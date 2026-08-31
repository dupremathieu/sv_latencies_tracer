/* SPDX-License-Identifier: Apache-2.0 */
#include "metrics.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

void metrics_init_with_max(struct sv_metrics_state *ms, int histogram_max_us)
{
	memset(ms, 0, sizeof(*ms));
	if (histogram_max_us < 0)
		histogram_max_us = 0;
	if (histogram_max_us > SV_HISTOGRAM_MAX_US)
		histogram_max_us = SV_HISTOGRAM_MAX_US;
	ms->histogram_max_us = histogram_max_us;
	pthread_mutex_init(&ms->stream_lock, NULL);
	pthread_mutex_init(&ms->interval_lock, NULL);
	atomic_store(&ms->link_up, 1);
}

void metrics_init(struct sv_metrics_state *ms)
{
	metrics_init_with_max(ms, SV_HISTOGRAM_MAX_US);
}

struct sv_stream_metrics *metrics_get_stream(struct sv_metrics_state *ms,
					     uint16_t app_id,
					     const char *sv_id)
{
	pthread_mutex_lock(&ms->stream_lock);
	for (int i = 0; i < ms->num_streams; i++) {
		struct sv_stream_metrics *s = &ms->streams[i];
		if (s->active && s->id.app_id == app_id &&
		    strcmp(s->id.sv_id, sv_id) == 0) {
			pthread_mutex_unlock(&ms->stream_lock);
			return s;
		}
	}

	if (ms->num_streams >= SV_MAX_STREAMS) {
		pthread_mutex_unlock(&ms->stream_lock);
		return NULL;
	}

	struct sv_stream_metrics *s = &ms->streams[ms->num_streams++];
	s->id.app_id = app_id;
	sv_copy_svid(s->id.sv_id, sv_id);
	histogram_init_with_max(&s->capture_latency, ms->histogram_max_us);
	histogram_init_with_max(&s->parsed_latency, ms->histogram_max_us);
	histogram_init_with_max(&s->hw_to_app_latency, ms->histogram_max_us);
	histogram_init_with_max(&s->sw_to_app_latency, ms->histogram_max_us);
	histogram_init_with_max(&s->hw_to_sw_latency, ms->histogram_max_us);
	histogram_init_with_max(&s->interval_hw, ms->histogram_max_us);
	histogram_init_with_max(&s->interval_app, ms->histogram_max_us);
	if (!s->capture_latency.buckets || !s->parsed_latency.buckets ||
	    !s->hw_to_app_latency.buckets || !s->sw_to_app_latency.buckets ||
	    !s->hw_to_sw_latency.buckets ||
	    !s->interval_hw.buckets || !s->interval_app.buckets) {
		histogram_destroy(&s->capture_latency);
		histogram_destroy(&s->parsed_latency);
		histogram_destroy(&s->hw_to_app_latency);
		histogram_destroy(&s->sw_to_app_latency);
		histogram_destroy(&s->hw_to_sw_latency);
		histogram_destroy(&s->interval_hw);
		histogram_destroy(&s->interval_app);
		ms->num_streams--;
		pthread_mutex_unlock(&ms->stream_lock);
		return NULL;
	}
	atomic_store(&s->interval_hw_current_ns, 0);
	atomic_store(&s->interval_app_current_ns, 0);
	atomic_store(&s->timestamp_hardware_total, 0);
	atomic_store(&s->timestamp_software_total, 0);
	atomic_store(&s->timestamp_application_total, 0);
	s->have_prev = 0;
	s->active = 1;
	pthread_mutex_unlock(&ms->stream_lock);
	return s;
}

int metrics_record_interval(struct sv_metrics_state *ms, uint16_t app_id,
			    const char *sv_id, uint16_t smp_cnt,
			    const struct sv_timestamp *rx_ts,
			    const struct sv_timestamp *app_ts,
			    enum sv_timestamp_source source)
{
	struct sv_stream_metrics *s =
		metrics_get_stream(ms, app_id, sv_id);
	if (!s)
		return -1;

	pthread_mutex_lock(&ms->interval_lock);
	if (s->have_prev && s->last_timestamp_source == source) {
		int64_t interval_hw_ns = ts_delta_ns(rx_ts, &s->last_rx_ts);
		int64_t interval_app_ns = ts_delta_ns(app_ts, &s->last_app_ts);

		atomic_store_explicit(&s->interval_hw_current_ns, interval_hw_ns,
				      memory_order_relaxed);
		atomic_store_explicit(&s->interval_app_current_ns, interval_app_ns,
				      memory_order_relaxed);
		histogram_record(&s->interval_hw, interval_hw_ns / 1000);
		histogram_record(&s->interval_app, interval_app_ns / 1000);
	}
	s->last_smp_cnt = smp_cnt;
	s->last_rx_ts = *rx_ts;
	s->last_app_ts = *app_ts;
	s->last_timestamp_source = source;
	s->have_prev = 1;
	pthread_mutex_unlock(&ms->interval_lock);

	return 0;
}

void metrics_record_timestamp_source(struct sv_stream_metrics *stream,
				     enum sv_timestamp_source source)
{
	_Atomic uint64_t *counter;

	switch (source) {
	case SV_TIMESTAMP_SOURCE_HARDWARE:
		counter = &stream->timestamp_hardware_total;
		break;
	case SV_TIMESTAMP_SOURCE_SOFTWARE:
		counter = &stream->timestamp_software_total;
		break;
	case SV_TIMESTAMP_SOURCE_APPLICATION:
		counter = &stream->timestamp_application_total;
		break;
	default:
		return;
	}
	atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static int append_histogram(char **buf, size_t *cap, size_t *pos,
			    const char *name, const char *help,
			    const struct sv_histogram *h,
			    uint16_t app_id, const char *sv_id)
{
	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	uint64_t overflow, sum, count;
	if (!buckets)
		return -1;
	histogram_snapshot(h, buckets, &overflow, &sum, &count);

	/* HELP and TYPE lines */
	int n = snprintf(*buf + *pos, *cap - *pos,
			 "# HELP %s %s\n"
			 "# TYPE %s histogram\n",
			 name, help, name);
	if (n < 0)
		goto fail;
	*pos += (size_t)n;

	/* Cumulative bucket lines */
	uint64_t cumulative = 0;
	int max_bucket = h->max_bucket_us;
	for (int i = 0; i <= max_bucket; i++) {
		cumulative += buckets[i];
		if (buckets[i] == 0)
			continue; /* sparse output */

		if (*pos + 256 > *cap) {
			*cap *= 2;
			char *nb = realloc(*buf, *cap);
			if (!nb)
				goto fail;
			*buf = nb;
		}
		n = snprintf(*buf + *pos, *cap - *pos,
			     "%s_bucket{appid=\"0x%04X\",svid=\"%s\","
			     "le=\"%d\"} %lu\n",
			     name, app_id, sv_id, i, (unsigned long)cumulative);
		if (n < 0)
			goto fail;
		*pos += (size_t)n;
	}

	/* +Inf bucket */
	cumulative += overflow;
	if (*pos + 512 > *cap) {
		*cap *= 2;
		char *nb = realloc(*buf, *cap);
		if (!nb)
			goto fail;
		*buf = nb;
	}
	n = snprintf(*buf + *pos, *cap - *pos,
		     "%s_bucket{appid=\"0x%04X\",svid=\"%s\","
		     "le=\"+Inf\"} %lu\n"
		     "%s_sum{appid=\"0x%04X\",svid=\"%s\"} %lu\n"
		     "%s_count{appid=\"0x%04X\",svid=\"%s\"} %lu\n",
		     name, app_id, sv_id, (unsigned long)cumulative,
		     name, app_id, sv_id, (unsigned long)sum,
		     name, app_id, sv_id, (unsigned long)count);
	if (n < 0)
		goto fail;
	*pos += (size_t)n;
	free(buckets);
	return 0;

	fail:
	free(buckets);
	return -1;
}

static int append_max(char **buf, size_t *cap, size_t *pos,
		      const char *name, const struct sv_histogram *h,
		      uint16_t app_id, const char *sv_id)
{
	if (*pos + 512 > *cap) {
		*cap *= 2;
		char *nb = realloc(*buf, *cap);
		if (!nb)
			return -1;
		*buf = nb;
	}

	int n = snprintf(*buf + *pos, *cap - *pos,
			 "# HELP %s Maximum observed RX-timestamp-to-application latency (us)\n"
			 "# TYPE %s gauge\n"
			 "%s{appid=\"0x%04X\",svid=\"%s\"} %lu\n",
			 name, name, name, app_id, sv_id,
			 (unsigned long)histogram_max(h));
	if (n < 0)
		return -1;
	*pos += (size_t)n;
	return 0;
}

static int append_observation_counts(char **buf, size_t *cap, size_t *pos,
				      const char *name,
				      const struct sv_histogram *h,
				      uint16_t app_id, const char *sv_id)
{
	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	uint64_t overflow, sum, count;
	struct sv_histogram_observation *exact_overflow = NULL;
	size_t exact_overflow_count = 0;
	if (!buckets)
		return -1;
	histogram_snapshot(h, buckets, &overflow, &sum, &count);
	if (histogram_snapshot_exact(h, &exact_overflow,
					     &exact_overflow_count) < 0)
		goto fail;

	int n = snprintf(*buf + *pos, *cap - *pos,
			 "# HELP %s Number of observations at each exact latency (us)\n"
			 "# TYPE %s counter\n", name, name);
	if (n < 0)
		goto fail;
	*pos += (size_t)n;

	int max_bin = -1;
	for (int i = h->max_bucket_us; i >= 0; i--) {
		if (buckets[i] > 0) {
			max_bin = i;
			break;
		}
	}

	for (int i = 0; i <= max_bin; i++) {
		if (buckets[i] == 0)
			continue; /* Keep exact output sparse. */

		if (*pos + 256 > *cap) {
			*cap *= 2;
			char *nb = realloc(*buf, *cap);
			if (!nb)
				goto fail;
			*buf = nb;
		}
		n = snprintf(*buf + *pos, *cap - *pos,
			     "%s{appid=\"0x%04X\",svid=\"%s\","
			     "latency_us=\"%d\"} %lu\n",
			     name, app_id, sv_id, i,
			     (unsigned long)buckets[i]);
		if (n < 0)
			goto fail;
		*pos += (size_t)n;
	}

	for (size_t i = 0; i < exact_overflow_count; i++) {
		if (*pos + 256 > *cap) {
			*cap *= 2;
			char *nb = realloc(*buf, *cap);
			if (!nb)
				goto fail;
			*buf = nb;
		}
		n = snprintf(*buf + *pos, *cap - *pos,
			     "%s{appid=\"0x%04X\",svid=\"%s\","
			     "latency_us=\"%lu\"} %lu\n",
			     name, app_id, sv_id,
			     (unsigned long)exact_overflow[i].latency_us,
			     (unsigned long)exact_overflow[i].count);
		if (n < 0)
			goto fail;
		*pos += (size_t)n;
	}

	free(buckets);
	free(exact_overflow);
	return 0;

fail:
	free(buckets);
	free(exact_overflow);
	return -1;
}

char *metrics_format(const struct sv_metrics_state *ms,
		     const struct sv_drop_tracker *dt)
{
	const struct sv_stream_metrics *streams[SV_MAX_STREAMS];
	int num_streams = 0;
	size_t cap = 65536;
	size_t pos = 0;
	char *buf = malloc(cap);
	int dt_locked = 0;
	if (!buf)
		return NULL;
	buf[0] = '\0';

	/*
	 * Stream entries are immutable after publication and are never removed.
	 * Do not hold this lock while snapshotting the large atomic histograms:
	 * the capture path takes it once per received frame.
	 */
	pthread_mutex_lock((pthread_mutex_t *)&ms->stream_lock);
	for (int i = 0; i < ms->num_streams; i++) {
		if (ms->streams[i].active)
			streams[num_streams++] = &ms->streams[i];
	}
	pthread_mutex_unlock((pthread_mutex_t *)&ms->stream_lock);

	/* Per-stream histograms and counters */
	for (int i = 0; i < num_streams; i++) {
		const struct sv_stream_metrics *s = streams[i];
		uint16_t aid = s->id.app_id;
		const char *sid = s->id.sv_id;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_capture_latency_us",
				     "Latency from selected RX timestamp to app delivery (us)",
				     &s->capture_latency, aid, sid) < 0)
			goto fail;
		if (append_observation_counts(&buf, &cap, &pos,
					      "sv_capture_latency_us_observations_total",
					      &s->capture_latency, aid, sid) < 0)
			goto fail;
		if (append_max(&buf, &cap, &pos,
			       "sv_capture_latency_us_max",
			       &s->capture_latency, aid, sid) < 0)
			goto fail;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_parsed_latency_us",
				     "Latency from selected RX timestamp to post-parse (us)",
				     &s->parsed_latency, aid, sid) < 0)
			goto fail;
		if (append_histogram(&buf, &cap, &pos,
				     "sv_hw_timestamp_to_app_latency_us",
				     "Latency from hardware RX timestamp to app PHC read (us)",
				     &s->hw_to_app_latency, aid, sid) < 0)
			goto fail;
		if (append_histogram(&buf, &cap, &pos,
				     "sv_sw_timestamp_to_app_latency_us",
				     "Latency from software RX timestamp to app realtime read (us)",
				     &s->sw_to_app_latency, aid, sid) < 0)
			goto fail;
		if (append_histogram(&buf, &cap, &pos,
				     "sv_hw_to_sw_estimated_latency_us",
				     "Estimated hardware-to-software RX latency from elapsed-duration difference (us)",
				     &s->hw_to_sw_latency, aid, sid) < 0)
			goto fail;

		if (pos + 1024 > cap) {
			cap += 1024;
			char *nb = realloc(buf, cap);
			if (!nb)
				goto fail;
			buf = nb;
		}
		int n = snprintf(buf + pos, cap - pos,
				 "# HELP sv_timestamp_source_frames_total Frames by selected timestamp source\n"
				 "# TYPE sv_timestamp_source_frames_total counter\n"
				 "sv_timestamp_source_frames_total{appid=\"0x%04X\",svid=\"%s\",source=\"hardware\"} %lu\n"
				 "sv_timestamp_source_frames_total{appid=\"0x%04X\",svid=\"%s\",source=\"software\"} %lu\n"
				 "sv_timestamp_source_frames_total{appid=\"0x%04X\",svid=\"%s\",source=\"application\"} %lu\n",
				 aid, sid, (unsigned long)atomic_load_explicit(
					 &s->timestamp_hardware_total,
					 memory_order_relaxed),
				 aid, sid, (unsigned long)atomic_load_explicit(
					 &s->timestamp_software_total,
					 memory_order_relaxed),
				 aid, sid, (unsigned long)atomic_load_explicit(
					 &s->timestamp_application_total,
					 memory_order_relaxed));
		if (n < 0)
			goto fail;
		pos += (size_t)n;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_sv_interval_hw_us",
				     "Interval between consecutive SV frames (selected RX timestamp)",
				     &s->interval_hw, aid, sid) < 0)
			goto fail;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_sv_interval_app_us",
				     "Interval between consecutive SV frames (app timestamp)",
				     &s->interval_app, aid, sid) < 0)
			goto fail;

		if (pos + 1024 > cap) {
			cap += 1024;
			char *nb = realloc(buf, cap);
			if (!nb)
				goto fail;
			buf = nb;
		}
		int64_t hw_ns = atomic_load_explicit(
			&s->interval_hw_current_ns, memory_order_relaxed);
		int64_t app_ns = atomic_load_explicit(
			&s->interval_app_current_ns, memory_order_relaxed);
		n = snprintf(buf + pos, cap - pos,
				 "# HELP sv_sv_interval_hw_current_us Latest interval between two received SV frames (selected RX timestamp)\n"
				 "# TYPE sv_sv_interval_hw_current_us gauge\n"
				 "sv_sv_interval_hw_current_us{appid=\"0x%04X\",svid=\"%s\"} %.3f\n"
				 "# HELP sv_sv_interval_app_current_us Latest interval between two received SV frames (app timestamp)\n"
				 "# TYPE sv_sv_interval_app_current_us gauge\n"
				 "sv_sv_interval_app_current_us{appid=\"0x%04X\",svid=\"%s\"} %.3f\n",
				 aid, sid, (double)hw_ns / 1000.0,
				 aid, sid, (double)app_ns / 1000.0);
		if (n < 0)
			goto fail;
		pos += (size_t)n;
	}

	/* Drop tracker counters */
	if (dt) {
		pthread_mutex_lock((pthread_mutex_t *)&dt->lock);
		dt_locked = 1;
		int n;
		n = snprintf(buf + pos, cap - pos,
			     "# HELP sv_frames_total Total SV frames received\n"
			     "# TYPE sv_frames_total counter\n");
		if (n > 0)
			pos += (size_t)n;

		for (int i = 0; i < dt->num_streams; i++) {
			const struct sv_drop_state *ds = &dt->streams[i];
			if (pos + 256 > cap) {
				cap *= 2;
				char *nb = realloc(buf, cap);
				if (!nb)
					goto fail;
				buf = nb;
			}
			n = snprintf(buf + pos, cap - pos,
				     "sv_frames_total{appid=\"0x%04X\","
				     "svid=\"%s\"} %lu\n",
				     ds->id.app_id, ds->id.sv_id,
				     (unsigned long)atomic_load_explicit(
					     &ds->frames_received,
					     memory_order_relaxed));
			if (n > 0)
				pos += (size_t)n;
		}

		n = snprintf(buf + pos, cap - pos,
			     "# HELP sv_drops_total Total dropped SV frames\n"
			     "# TYPE sv_drops_total counter\n");
		if (n > 0)
			pos += (size_t)n;

		for (int i = 0; i < dt->num_streams; i++) {
			const struct sv_drop_state *ds = &dt->streams[i];
			if (pos + 256 > cap) {
				cap *= 2;
				char *nb = realloc(buf, cap);
				if (!nb)
					goto fail;
				buf = nb;
			}
			n = snprintf(buf + pos, cap - pos,
				     "sv_drops_total{appid=\"0x%04X\","
				     "svid=\"%s\"} %lu\n",
				     ds->id.app_id, ds->id.sv_id,
				     (unsigned long)atomic_load_explicit(
					     &ds->frames_dropped,
					     memory_order_relaxed));
			if (n > 0)
				pos += (size_t)n;
		}
		pthread_mutex_unlock((pthread_mutex_t *)&dt->lock);
		dt_locked = 0;
	}

	/* System metrics */
	if (pos + 1024 > cap) {
		cap += 1024;
		char *nb = realloc(buf, cap);
		if (!nb)
			goto fail;
		buf = nb;
	}

	int n = snprintf(buf + pos, cap - pos,
			 "# HELP sv_link_up Network link state (1=up)\n"
			 "# TYPE sv_link_up gauge\n"
			 "sv_link_up %d\n"
			 "# HELP sv_kernel_oops_total Kernel oops events\n"
			 "# TYPE sv_kernel_oops_total counter\n"
			 "sv_kernel_oops_total %lu\n"
			 "# HELP sv_kernel_panic_total Kernel panic events\n"
			 "# TYPE sv_kernel_panic_total counter\n"
			 "sv_kernel_panic_total %lu\n"
			 "# HELP sv_rt_throttle_total RT throttle events\n"
			 "# TYPE sv_rt_throttle_total counter\n"
			 "sv_rt_throttle_total %lu\n",
			 atomic_load_explicit(&ms->link_up,
					      memory_order_relaxed),
			 (unsigned long)atomic_load_explicit(
				 &ms->kernel_oops_total,
				 memory_order_relaxed),
			 (unsigned long)atomic_load_explicit(
				 &ms->kernel_panic_total,
				 memory_order_relaxed),
			 (unsigned long)atomic_load_explicit(
				 &ms->rt_throttle_total,
				 memory_order_relaxed));
	if (n > 0)
		pos += (size_t)n;

	return buf;

fail:
	if (dt_locked)
		pthread_mutex_unlock((pthread_mutex_t *)&dt->lock);
	free(buf);
	return NULL;
}

/* Minimal HTTP server for /metrics */

	struct metrics_server_ctx {
	int listen_fd;
	struct sv_metrics_state *ms;
	struct sv_drop_tracker *dt;
	pthread_t thread;
	_Atomic int running;
	int thread_started;
};

static struct metrics_server_ctx g_server;

static void handle_client(int client_fd, struct sv_metrics_state *ms,
			  struct sv_drop_tracker *dt)
{
	char req_buf[1024];
	struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		   sizeof(timeout));
	ssize_t n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
	if (n <= 0) {
		close(client_fd);
		return;
	}
	req_buf[n] = '\0';

	/* Only respond to GET /metrics */
	if (strncmp(req_buf, "GET /metrics", 12) != 0) {
		const char *resp = "HTTP/1.1 404 Not Found\r\n"
				   "Content-Length: 0\r\n\r\n";
		send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
		close(client_fd);
		return;
	}

	char *body = metrics_format(ms, dt);
	if (!body) {
		const char *resp = "HTTP/1.1 500 Internal Server Error\r\n"
				   "Content-Length: 0\r\n\r\n";
		send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
		close(client_fd);
		return;
	}

	size_t body_len = strlen(body);
	char hdr[256];
	int hdr_len = snprintf(hdr, sizeof(hdr),
			       "HTTP/1.1 200 OK\r\n"
			       "Content-Type: text/plain; version=0.0.4; "
			       "charset=utf-8\r\n"
			       "Content-Length: %zu\r\n"
			       "Connection: close\r\n\r\n",
			       body_len);

	send(client_fd, hdr, (size_t)hdr_len, MSG_NOSIGNAL);
	send(client_fd, body, body_len, MSG_NOSIGNAL);
	free(body);
	close(client_fd);
}

static void *metrics_server_thread(void *arg)
{
	struct metrics_server_ctx *ctx = arg;

	while (atomic_load_explicit(&ctx->running, memory_order_relaxed)) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_fd = accept(ctx->listen_fd,
				       (struct sockaddr *)&client_addr,
				       &addr_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		handle_client(client_fd, ctx->ms, ctx->dt);
	}
	return NULL;
}

int metrics_server_start(uint16_t port, struct sv_metrics_state *ms,
			 struct sv_drop_tracker *dt)
{
	g_server.listen_fd = -1;
	g_server.thread_started = 0;
	g_server.ms = ms;
	g_server.dt = dt;

	g_server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_server.listen_fd < 0) {
		perror("metrics: socket");
		return -1;
	}

	int opt = 1;
	setsockopt(g_server.listen_fd, SOL_SOCKET, SO_REUSEADDR,
		   &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(port),
	};

	if (bind(g_server.listen_fd, (struct sockaddr *)&addr,
		 sizeof(addr)) < 0) {
		perror("metrics: bind");
		close(g_server.listen_fd);
		g_server.listen_fd = -1;
		return -1;
	}

	if (listen(g_server.listen_fd, 8) < 0) {
		perror("metrics: listen");
		close(g_server.listen_fd);
		g_server.listen_fd = -1;
		return -1;
	}

	atomic_store_explicit(&g_server.running, 1, memory_order_relaxed);
	if (pthread_create(&g_server.thread, NULL, metrics_server_thread,
			   &g_server) != 0) {
		perror("metrics: pthread_create");
		close(g_server.listen_fd);
		g_server.listen_fd = -1;
		return -1;
	}
	g_server.thread_started = 1;

	return 0;
}

void metrics_server_stop(void)
{
	atomic_store_explicit(&g_server.running, 0, memory_order_relaxed);
	if (g_server.listen_fd >= 0) {
		shutdown(g_server.listen_fd, SHUT_RDWR);
		close(g_server.listen_fd);
		g_server.listen_fd = -1;
	}
	if (g_server.thread_started) {
		pthread_join(g_server.thread, NULL);
		g_server.thread_started = 0;
	}
}
