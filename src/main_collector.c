/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "drop_detector.h"
#include "histogram.h"
#include "metrics.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/*
 * Correlation buffer entry.
 * Keyed by (app_id, sv_id, smp_cnt). Stores HW timestamp from the agent.
 * Expires after CORRELATION_TIMEOUT_SEC seconds.
 */
#define CORRELATION_TIMEOUT_SEC 5
#define CORRELATION_TABLE_SIZE  65536

struct correlation_entry {
	uint16_t          app_id;
	char              sv_id[SV_SVID_MAX_LEN];
	uint16_t          smp_cnt;
	struct sv_timestamp hw_ts;
	time_t            inserted_at;
	int               occupied;
};

struct collector_state {
	struct sv_metrics_state metrics;
	struct sv_drop_tracker  drops;
	struct correlation_entry correlation[CORRELATION_TABLE_SIZE];
	_Atomic uint64_t correlation_failures;
	pthread_mutex_t lock;
};

static uint32_t correlation_hash(uint16_t app_id, const char *sv_id,
				 uint16_t smp_cnt)
{
	uint32_t h = (uint32_t)app_id * 31 + smp_cnt;
	for (const char *p = sv_id; *p; p++)
		h = h * 31 + (uint32_t)*p;
	return h % CORRELATION_TABLE_SIZE;
}

static void collector_insert_agent(struct collector_state *st,
				   const struct proto_agent_record *r)
{
	uint32_t idx = correlation_hash(r->app_id, r->sv_id, r->smp_cnt);

	pthread_mutex_lock(&st->lock);
	struct correlation_entry *e = &st->correlation[idx];
	e->app_id = r->app_id;
	sv_copy_svid(e->sv_id, r->sv_id);
	e->smp_cnt = r->smp_cnt;
	e->hw_ts = r->hw_ts;
	e->inserted_at = time(NULL);
	e->occupied = 1;
	pthread_mutex_unlock(&st->lock);
}

static void collector_match_subscriber(struct collector_state *st,
				       const struct proto_subscriber_record *r)
{
	uint32_t idx = correlation_hash(r->app_id, r->sv_id, r->smp_cnt);

	pthread_mutex_lock(&st->lock);
	struct correlation_entry *e = &st->correlation[idx];

	if (!e->occupied || e->app_id != r->app_id ||
	    strcmp(e->sv_id, r->sv_id) != 0 || e->smp_cnt != r->smp_cnt) {
		pthread_mutex_unlock(&st->lock);
		atomic_fetch_add_explicit(&st->correlation_failures, 1,
					 memory_order_relaxed);
		return;
	}

	/* Check timeout */
	if (time(NULL) - e->inserted_at > CORRELATION_TIMEOUT_SEC) {
		e->occupied = 0;
		pthread_mutex_unlock(&st->lock);
		atomic_fetch_add_explicit(&st->correlation_failures, 1,
					 memory_order_relaxed);
		return;
	}

	struct sv_timestamp hw_ts = e->hw_ts;
	e->occupied = 0;
	pthread_mutex_unlock(&st->lock);

	int64_t delta_capture = ts_delta_us(&r->app_ts, &hw_ts);
	int64_t delta_parsed = ts_delta_us(&r->parsed_ts, &hw_ts);

	struct sv_stream_metrics *sm =
		metrics_get_stream(&st->metrics, r->app_id, r->sv_id);
	if (sm) {
		histogram_record(&sm->capture_latency, delta_capture);
		histogram_record(&sm->parsed_latency, delta_parsed);
	}

	/* Track drops via subscriber records */
	struct sv_frame_info info = {
		.app_id = r->app_id,
		.smp_cnt = r->smp_cnt,
	};
	sv_copy_svid(info.sv_id, r->sv_id);
	drop_tracker_process(&st->drops, &info);
}

struct client_thread_args {
	int client_fd;
	struct collector_state *state;
};

static void *client_handler(void *arg)
{
	struct client_thread_args *a = arg;
	int fd = a->client_fd;
	struct collector_state *st = a->state;
	free(a);

	while (g_running) {
		uint8_t *buf;
		ssize_t len = proto_recv_batch(fd, &buf);
		if (len <= 0)
			break;

		uint8_t src_type;
		struct proto_agent_record agent_recs[512];
		struct proto_subscriber_record sub_recs[512];

		int count = proto_deserialize_batch(buf, (size_t)len,
						    &src_type,
						    agent_recs, sub_recs,
						    512);
		free(buf);

		if (count < 0)
			continue;

		if (src_type == PROTO_SRC_AGENT) {
			for (int i = 0; i < count; i++)
				collector_insert_agent(st, &agent_recs[i]);
		} else if (src_type == PROTO_SRC_SUBSCRIBER) {
			for (int i = 0; i < count; i++)
				collector_match_subscriber(st, &sub_recs[i]);
		}
	}

	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	struct sv_config cfg;
	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_COLLECTOR;

	if (config_parse_args(&cfg, argc, argv) < 0)
		return 1;

	struct sigaction sa = {
		.sa_handler = signal_handler,
		.sa_flags = 0,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	struct collector_state state;
	memset(&state, 0, sizeof(state));
	metrics_init(&state.metrics);
	drop_tracker_init(&state.drops);
	pthread_mutex_init(&state.lock, NULL);

	if (metrics_server_start(cfg.prometheus_port, &state.metrics,
				 &state.drops) < 0)
		return 1;

	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}

	int opt = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(cfg.collector_port),
	};
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(listen_fd);
		return 1;
	}
	if (listen(listen_fd, 4) < 0) {
		perror("listen");
		close(listen_fd);
		return 1;
	}

	fprintf(stderr,
		"sv-collector: listening on :%u, metrics on :%u\n",
		cfg.collector_port, cfg.prometheus_port);

	while (g_running) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_fd = accept(listen_fd,
				       (struct sockaddr *)&client_addr,
				       &addr_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		fprintf(stderr, "collector: client connected from %s:%d\n",
			inet_ntoa(client_addr.sin_addr),
			ntohs(client_addr.sin_port));

		struct client_thread_args *a = malloc(sizeof(*a));
		a->client_fd = client_fd;
		a->state = &state;

		pthread_t th;
		if (pthread_create(&th, NULL, client_handler, a) != 0) {
			free(a);
			close(client_fd);
		} else {
			pthread_detach(th);
		}
	}

	close(listen_fd);
	metrics_server_stop();
	pthread_mutex_destroy(&state.lock);
	return 0;
}
