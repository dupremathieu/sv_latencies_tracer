/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "drop_detector.h"
#include "frame_capture.h"
#include "histogram.h"
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

/*
 * Direct mode (Scenario A): capture, parse, measure, record.
 */
static int run_direct(const struct sv_config *cfg)
{
	struct sv_capture_ctx capture;
	struct sv_metrics_state metrics;
	struct sv_drop_tracker drops;
	struct sv_sysmon_ctx sysmon;

	metrics_init(&metrics);
	drop_tracker_init(&drops);

	const char *phc = cfg->phc_device_set ? cfg->phc_device : NULL;
	if (capture_open(&capture, cfg->interface, phc) < 0)
		return 1;

	if (metrics_server_start(cfg->prometheus_port, &metrics, &drops) < 0) {
		capture_close(&capture);
		return 1;
	}

	sysmon_start(&sysmon, &metrics, cfg->interface);

	fprintf(stderr, "sv-subscriber: direct mode on %s, metrics on :%u\n",
		cfg->interface, cfg->prometheus_port);

	struct sv_captured_frame frame;
	while (g_running) {
		if (capture_recv(&capture, &frame) < 0) {
			if (errno == EINTR)
				continue;
			perror("capture_recv");
			break;
		}

		struct sv_frame_info info;
		if (sv_parse(frame.data, frame.len, &info) < 0)
			continue;

		/* Record T_parsed */
		struct timespec parsed_now;
		clock_gettime(capture.phc_clockid, &parsed_now);
		struct sv_timestamp parsed_ts = timespec_to_svts(&parsed_now);

		/* Compute deltas */
		int64_t delta_capture = ts_delta_us(&frame.app_ts, &frame.hw_ts);
		int64_t delta_parsed = ts_delta_us(&parsed_ts, &frame.hw_ts);

		/* Record in histograms */
		struct sv_stream_metrics *sm =
			metrics_get_stream(&metrics, info.app_id, info.sv_id);
		if (sm) {
			histogram_record(&sm->capture_latency, delta_capture);
			histogram_record(&sm->parsed_latency, delta_parsed);
		}

		/* Track drops */
		drop_tracker_process(&drops, &info);
	}

	fprintf(stderr, "\nShutting down...\n");
	sysmon_stop(&sysmon);
	metrics_server_stop();
	capture_close(&capture);
	return 0;
}

/*
 * Split mode (Scenario B, VM side): capture, parse, send to collector.
 */
static int run_split_subscriber(const struct sv_config *cfg)
{
	struct sv_capture_ctx capture;
	struct sv_drop_tracker drops;

	drop_tracker_init(&drops);

	const char *phc = cfg->phc_device_set ? cfg->phc_device : NULL;
	if (capture_open(&capture, cfg->interface, phc) < 0)
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
		clock_gettime(capture.phc_clockid, &parsed_now);
		struct sv_timestamp parsed_ts = timespec_to_svts(&parsed_now);

		drop_tracker_process(&drops, &info);

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
				proto_send_batch(sock, buf, (size_t)len);
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
			proto_send_batch(sock, buf, (size_t)len);
			free(buf);
		}
	}

	free(batch);
	close(sock);
	capture_close(&capture);
	return 0;
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

	apply_rt_settings(&cfg);

	if (cfg.mode == SV_MODE_DIRECT)
		return run_direct(&cfg);
	else
		return run_split_subscriber(&cfg);
}
