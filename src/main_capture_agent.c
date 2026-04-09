/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "frame_capture.h"
#include "protocol.h"
#include "sv_parser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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

int main(int argc, char **argv)
{
	struct sv_config cfg;
	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_CAPTURE_AGENT;

	if (config_parse_args(&cfg, argc, argv) < 0)
		return 1;

	struct sigaction sa = {
		.sa_handler = signal_handler,
		.sa_flags = 0,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	struct sv_capture_ctx capture;
	const char *phc = cfg.phc_device_set ? cfg.phc_device : NULL;
	if (capture_open(&capture, cfg.interface, phc) < 0)
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
		.sin_port = htons(cfg.collector_port),
	};
	if (inet_pton(AF_INET, cfg.collector_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "Invalid collector address: %s\n",
			cfg.collector_addr);
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
		"sv-capture-agent: capturing on %s, collector %s:%u\n",
		cfg.interface, cfg.collector_addr, cfg.collector_port);

	struct proto_agent_record *batch =
		calloc((size_t)cfg.batch_size,
		       sizeof(struct proto_agent_record));
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

		struct proto_agent_record *r = &batch[batch_idx];
		r->app_id = info.app_id;
		sv_copy_svid(r->sv_id, info.sv_id);
		r->smp_cnt = info.smp_cnt;
		r->hw_ts = frame.hw_ts;
		batch_idx++;

		if (batch_idx >= cfg.batch_size) {
			uint8_t *buf;
			ssize_t len = proto_serialize_agent_batch(
				batch, batch_idx, &buf);
			if (len > 0) {
				proto_send_batch(sock, buf, (size_t)len);
				free(buf);
			}
			batch_idx = 0;
		}
	}

	/* Flush */
	if (batch_idx > 0) {
		uint8_t *buf;
		ssize_t len = proto_serialize_agent_batch(
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
