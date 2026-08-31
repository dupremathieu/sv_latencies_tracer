/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "drop_detector.h"
#include "frame_capture.h"
#include "histogram.h"
#include "live_histogram.h"
#include "metrics.h"
#include "protocol.h"
#include "sv_parser.h"
#include "system_monitor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void apply_rt_settings(const struct sv_config *cfg)
{
	if (cfg->cpu_affinity >= 0) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(cfg->cpu_affinity, &cpuset);
		if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
			perror("sched_setaffinity");
		else
			fprintf(stderr, "Pinned to CPU %d\n",
				cfg->cpu_affinity);
	}

	if (cfg->sched_fifo) {
		struct sched_param sp = { .sched_priority = cfg->sched_priority };
		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
			perror("sched_setscheduler SCHED_FIFO");
		else
			fprintf(stderr, "SCHED_FIFO priority %d\n",
				cfg->sched_priority);
	}
}

static int warmup_active(const struct timespec *deadline)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;

	return now.tv_sec < deadline->tv_sec ||
		(now.tv_sec == deadline->tv_sec && now.tv_nsec < deadline->tv_nsec);
}

static const char *timestamp_source_name(enum sv_timestamp_source source)
{
	switch (source) {
	case SV_TIMESTAMP_SOURCE_HARDWARE:
		return "hardware";
	case SV_TIMESTAMP_SOURCE_SOFTWARE:
		return "software";
	case SV_TIMESTAMP_SOURCE_APPLICATION:
		return "application";
	}
	return "unknown";
}

static FILE *open_output_file(const struct sv_config *cfg)
{
	if (!cfg->output_path_set)
		return NULL;

	FILE *output = fopen(cfg->output_path, "w");
	if (!output) {
		fprintf(stderr, "Cannot open output file '%s': %s\n",
			cfg->output_path, strerror(errno));
		return NULL;
	}

	if (fprintf(output,
		    "# sv_latencies_tracer direct samples\n"
		    "# app_id sv_id smp_cnt timestamp_source "
		    "rx_sec rx_nsec app_sec app_nsec parsed_sec parsed_nsec "
		    "capture_latency_us parsed_latency_us\n") < 0) {
		fprintf(stderr, "Cannot write output file '%s': %s\n",
			cfg->output_path, strerror(errno));
		fclose(output);
		return NULL;
	}
	return output;
}

static void close_output_file(FILE *output)
{
	if (output)
		fclose(output);
}

static int write_output_sample(FILE *output, const struct sv_frame_info *info,
			       const struct sv_captured_frame *frame,
			       const struct sv_timestamp *parsed_ts,
			       int64_t capture_latency_us,
			       int64_t parsed_latency_us)
{
	if (!output)
		return 0;

	int result = fprintf(output,
		"0x%04" PRIx16 "\t%s\t%" PRIu16 "\t%s\t"
		"%" PRIu64 "\t%" PRIu32 "\t"
		"%" PRIu64 "\t%" PRIu32 "\t"
		"%" PRIu64 "\t%" PRIu32 "\t"
		"%" PRId64 "\t%" PRId64 "\n",
		info->app_id, info->sv_id, info->smp_cnt,
		timestamp_source_name(frame->timestamp_source),
		frame->rx_ts.sec, frame->rx_ts.nsec,
		frame->app_ts.sec, frame->app_ts.nsec,
		parsed_ts->sec, parsed_ts->nsec,
		capture_latency_us, parsed_latency_us);
	return result < 0 ? -1 : 0;
}

/*
 * Direct mode (Scenario A): capture, parse, measure, record.
 */
static int run_direct(const struct sv_config *cfg)
{
	struct sv_capture_ctx capture;
	struct sv_metrics_state metrics;
	struct sv_drop_tracker drops;
	struct sv_sysmon_ctx sysmon;
	struct sv_live_histogram_ctx live_histogram;
	FILE *output = open_output_file(cfg);
	if (cfg->output_path_set && !output)
		return 1;

	metrics_init_with_max(&metrics, cfg->histogram_max_us);
	drop_tracker_init(&drops);

	const char *phc = cfg->phc_device_set ? cfg->phc_device : NULL;
	if (capture_open(&capture, cfg->interface, phc, cfg->vlan_id,
			 cfg->enable_hw_timestamps) < 0) {
		close_output_file(output);
		return 1;
	}

	if (cfg->prometheus_enabled &&
	    metrics_server_start(cfg->prometheus_port, &metrics, &drops) < 0) {
		close_output_file(output);
		capture_close(&capture);
		return 1;
	}

	if (sysmon_start(&sysmon, &metrics, cfg->interface) < 0) {
		if (cfg->prometheus_enabled)
			metrics_server_stop();
		close_output_file(output);
		capture_close(&capture);
		return 1;
	}
	if (cfg->live_histogram &&
	    live_histogram_start(&live_histogram, &metrics,
				 cfg->live_threshold_us) < 0) {
		sysmon_stop(&sysmon);
		if (cfg->prometheus_enabled)
			metrics_server_stop();
		close_output_file(output);
		capture_close(&capture);
		return 1;
	}

	/* Keep monitoring and metrics threads off the real-time capture policy. */
	apply_rt_settings(cfg);
	struct timespec warmup_deadline;
	if (clock_gettime(CLOCK_MONOTONIC, &warmup_deadline) < 0) {
		perror("clock_gettime capture start");
		if (cfg->live_histogram)
			live_histogram_stop(&live_histogram);
		sysmon_stop(&sysmon);
		if (cfg->prometheus_enabled)
			metrics_server_stop();
		close_output_file(output);
		capture_close(&capture);
		return 1;
	}
	warmup_deadline.tv_sec += cfg->warmup_seconds;
	int warmup_done = cfg->warmup_seconds == 0;

	if (cfg->prometheus_enabled)
		fprintf(stderr,
			"sv-subscriber: direct mode on %s, metrics on :%u\n",
			cfg->interface, cfg->prometheus_port);
	else
		fprintf(stderr, "sv-subscriber: direct mode on %s\n",
			cfg->interface);

	struct sv_captured_frame frame;
	while (g_running) {
		if (capture_recv(&capture, &frame) < 0) {
			if (errno == EINTR)
				continue;
			perror("capture_recv");
			break;
		}
		if (!warmup_done) {
			if (warmup_active(&warmup_deadline))
				continue;
			warmup_done = 1;
		}

		struct sv_frame_info info;
		if (sv_parse(frame.data, frame.len, &info) < 0)
			continue;

		/* Record T_parsed */
		struct timespec parsed_now;
		if (clock_gettime(frame.timestamp_clockid, &parsed_now) < 0) {
			perror("clock_gettime parsed timestamp");
			break;
		}
		struct sv_timestamp parsed_ts = timespec_to_svts(&parsed_now);

		/* Compute deltas */
		int64_t delta_capture = ts_delta_us(&frame.app_ts, &frame.rx_ts);
		int64_t delta_parsed = ts_delta_us(&parsed_ts, &frame.rx_ts);

		/* Record in histograms */
		struct sv_stream_metrics *sm =
			metrics_get_stream(&metrics, info.app_id, info.sv_id);
		if (sm) {
			histogram_record(&sm->capture_latency, delta_capture);
			histogram_record(&sm->parsed_latency, delta_parsed);
			if (frame.have_hw_rx_ts && frame.have_app_phc_ts) {
				int64_t hw_to_app_ns = ts_delta_ns(
					&frame.app_phc_ts, &frame.hw_rx_ts);
				histogram_record(&sm->hw_to_app_latency,
						hw_to_app_ns / 1000);
				if (frame.have_sw_rx_ts) {
					int64_t sw_to_app_ns = ts_delta_ns(
						&frame.app_realtime_ts,
						&frame.sw_rx_ts);
					histogram_record(&sm->hw_to_sw_latency,
							(hw_to_app_ns -
							 sw_to_app_ns) / 1000);
				}
			}
			if (frame.have_sw_rx_ts) {
				int64_t sw_to_app_ns = ts_delta_ns(
					&frame.app_realtime_ts, &frame.sw_rx_ts);
				histogram_record(&sm->sw_to_app_latency,
						sw_to_app_ns / 1000);
			}
			metrics_record_timestamp_source(sm,
						frame.timestamp_source);
		}

		metrics_record_interval(&metrics, info.app_id, info.sv_id,
					info.smp_cnt, &frame.rx_ts,
					&frame.app_ts,
					frame.timestamp_source);

		/* Track drops */
		drop_tracker_process_at(&drops, &info, &frame.app_ts);
		if (write_output_sample(output, &info, &frame, &parsed_ts,
					 delta_capture, delta_parsed) < 0) {
			fprintf(stderr, "Cannot write output file '%s': %s\n",
				cfg->output_path, strerror(errno));
			break;
		}
	}

	fprintf(stderr, "\nShutting down...\n");
	if (cfg->live_histogram)
		live_histogram_stop(&live_histogram);
	sysmon_stop(&sysmon);
	if (cfg->prometheus_enabled)
		metrics_server_stop();
	capture_close(&capture);
	if (output && fclose(output) != 0)
		fprintf(stderr, "Cannot close output file '%s': %s\n",
			cfg->output_path, strerror(errno));
	return 0;
}

/*
 * Split mode (Scenario B, VM side): capture, parse, send to collector.
 */
static int run_split_subscriber(const struct sv_config *cfg)
{
	struct sv_capture_ctx capture;
	struct sv_drop_tracker drops;
	int status = 0;

	drop_tracker_init(&drops);

	const char *phc = cfg->phc_device_set ? cfg->phc_device : NULL;
	if (capture_open(&capture, cfg->interface, phc, cfg->vlan_id,
			 cfg->enable_hw_timestamps) < 0)
		return 1;

	/* Connect to collector */
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		capture_close(&capture);
		return 1;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(cfg->collector_port),
	};
	if (inet_pton(AF_INET, cfg->collector_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "Invalid collector address: %s\n",
			cfg->collector_addr);
		close(sock);
		capture_close(&capture);
		return 1;
	}
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect to collector");
		close(sock);
		capture_close(&capture);
		return 1;
	}

	fprintf(stderr,
		"sv-subscriber: split mode on %s, collector %s:%u\n",
		cfg->interface, cfg->collector_addr, cfg->collector_port);

	struct proto_subscriber_record *batch =
		calloc((size_t)cfg->batch_size,
		       sizeof(struct proto_subscriber_record));
	if (!batch) {
		close(sock);
		capture_close(&capture);
		return 1;
	}

	int batch_idx = 0;
	struct sv_captured_frame frame;
	apply_rt_settings(cfg);

	while (g_running) {
		if (capture_recv(&capture, &frame) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		struct sv_frame_info info;
		if (sv_parse(frame.data, frame.len, &info) < 0)
			continue;

		struct timespec parsed_now;
		if (clock_gettime(frame.timestamp_clockid, &parsed_now) < 0) {
			perror("clock_gettime parsed timestamp");
			break;
		}
		struct sv_timestamp parsed_ts = timespec_to_svts(&parsed_now);

		drop_tracker_process_at(&drops, &info, &frame.app_ts);

		struct proto_subscriber_record *r = &batch[batch_idx];
		r->app_id = info.app_id;
		sv_copy_svid(r->sv_id, info.sv_id);
		r->smp_cnt = info.smp_cnt;
		r->app_ts = frame.app_ts;
		r->parsed_ts = parsed_ts;
		batch_idx++;

		if (batch_idx >= cfg->batch_size) {
			uint8_t *buf;
			ssize_t len = proto_serialize_subscriber_batch(
				batch, batch_idx, &buf);
			if (len > 0) {
				if (proto_send_batch(sock, buf, (size_t)len) < 0) {
					perror("send batch to collector");
					free(buf);
					batch_idx = 0;
					status = 1;
					break;
				}
				free(buf);
			}
			batch_idx = 0;
		}
	}

	/* Flush remaining */
	if (batch_idx > 0) {
		uint8_t *buf;
		ssize_t len = proto_serialize_subscriber_batch(
			batch, batch_idx, &buf);
		if (len > 0) {
			if (proto_send_batch(sock, buf, (size_t)len) < 0) {
				perror("send final batch to collector");
				status = 1;
			}
			free(buf);
		}
	}

	free(batch);
	close(sock);
	capture_close(&capture);
	return status;
}

int main(int argc, char **argv)
{
	struct sv_config cfg;
	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_SUBSCRIBER;

	if (config_parse_args(&cfg, argc, argv) < 0)
		return 1;

	struct sigaction sa = {
		.sa_handler = signal_handler,
		.sa_flags = 0, /* no SA_RESTART — let blocking calls return EINTR */
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (cfg.mode == SV_MODE_DIRECT)
		return run_direct(&cfg);
	else
		return run_split_subscriber(&cfg);
}
