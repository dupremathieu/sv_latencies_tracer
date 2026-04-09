/* SPDX-License-Identifier: Apache-2.0 */
#include "metrics.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void metrics_init(struct sv_metrics_state *ms)
{
	memset(ms, 0, sizeof(*ms));
	atomic_store(&ms->link_up, 1);
}

struct sv_stream_metrics *metrics_get_stream(struct sv_metrics_state *ms,
					     uint16_t app_id,
					     const char *sv_id)
{
	for (int i = 0; i < ms->num_streams; i++) {
		struct sv_stream_metrics *s = &ms->streams[i];
		if (s->active && s->id.app_id == app_id &&
		    strcmp(s->id.sv_id, sv_id) == 0)
			return s;
	}

	if (ms->num_streams >= SV_MAX_STREAMS)
		return NULL;

	struct sv_stream_metrics *s = &ms->streams[ms->num_streams++];
	s->id.app_id = app_id;
	sv_copy_svid(s->id.sv_id, sv_id);
	histogram_init(&s->capture_latency);
	histogram_init(&s->parsed_latency);
	s->active = 1;
	return s;
}

static int append_histogram(char **buf, size_t *cap, size_t *pos,
			    const char *name, const char *help,
			    const struct sv_histogram *h,
			    uint16_t app_id, const char *sv_id)
{
	uint64_t buckets[SV_HISTOGRAM_BINS];
	uint64_t overflow, sum, count;
	histogram_snapshot(h, buckets, &overflow, &sum, &count);

	/* HELP and TYPE lines */
	int n = snprintf(*buf + *pos, *cap - *pos,
			 "# HELP %s %s\n"
			 "# TYPE %s histogram\n",
			 name, help, name);
	if (n < 0)
		return -1;
	*pos += (size_t)n;

	/* Cumulative bucket lines */
	uint64_t cumulative = 0;
	for (int i = 0; i < SV_HISTOGRAM_BINS; i++) {
		cumulative += buckets[i];
		if (buckets[i] == 0 && i < SV_HISTOGRAM_BINS - 1)
			continue; /* sparse output */

		if (*pos + 256 > *cap) {
			*cap *= 2;
			char *nb = realloc(*buf, *cap);
			if (!nb)
				return -1;
			*buf = nb;
		}
		n = snprintf(*buf + *pos, *cap - *pos,
			     "%s_bucket{appid=\"0x%04X\",svid=\"%s\","
			     "le=\"%d\"} %lu\n",
			     name, app_id, sv_id, i, (unsigned long)cumulative);
		if (n < 0)
			return -1;
		*pos += (size_t)n;
	}

	/* +Inf bucket */
	cumulative += overflow;
	if (*pos + 512 > *cap) {
		*cap *= 2;
		char *nb = realloc(*buf, *cap);
		if (!nb)
			return -1;
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
		return -1;
	*pos += (size_t)n;

	return 0;
}

char *metrics_format(const struct sv_metrics_state *ms,
		     const struct sv_drop_tracker *dt)
{
	size_t cap = 65536;
	size_t pos = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	buf[0] = '\0';

	/* Per-stream histograms and counters */
	for (int i = 0; i < ms->num_streams; i++) {
		const struct sv_stream_metrics *s = &ms->streams[i];
		if (!s->active)
			continue;

		uint16_t aid = s->id.app_id;
		const char *sid = s->id.sv_id;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_capture_latency_us",
				     "Latency from NIC HW TS to app delivery (us)",
				     &s->capture_latency, aid, sid) < 0)
			goto fail;

		if (append_histogram(&buf, &cap, &pos,
				     "sv_parsed_latency_us",
				     "Latency from NIC HW TS to post-parse (us)",
				     &s->parsed_latency, aid, sid) < 0)
			goto fail;
	}

	/* Drop tracker counters */
	if (dt) {
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
	free(buf);
	return NULL;
}

/* Minimal HTTP server for /metrics */

struct metrics_server_ctx {
	int listen_fd;
	struct sv_metrics_state *ms;
	struct sv_drop_tracker *dt;
	pthread_t thread;
	volatile int running;
};

static struct metrics_server_ctx g_server;

static void handle_client(int client_fd, struct sv_metrics_state *ms,
			  struct sv_drop_tracker *dt)
{
	char req_buf[1024];
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

	while (ctx->running) {
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
		return -1;
	}

	if (listen(g_server.listen_fd, 8) < 0) {
		perror("metrics: listen");
		close(g_server.listen_fd);
		return -1;
	}

	g_server.running = 1;
	if (pthread_create(&g_server.thread, NULL, metrics_server_thread,
			   &g_server) != 0) {
		perror("metrics: pthread_create");
		close(g_server.listen_fd);
		return -1;
	}

	return 0;
}

void metrics_server_stop(void)
{
	g_server.running = 0;
	if (g_server.listen_fd >= 0) {
		shutdown(g_server.listen_fd, SHUT_RDWR);
		close(g_server.listen_fd);
		g_server.listen_fd = -1;
	}
	pthread_join(g_server.thread, NULL);
}
