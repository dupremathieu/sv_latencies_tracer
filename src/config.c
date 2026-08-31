/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"
#include "common.h"

#include <getopt.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_int(const char *text, int min, int max, int *value)
{
	char *end;
	errno = 0;
	long parsed = strtol(text, &end, 10);
	if (errno == ERANGE || *text == '\0' || *end != '\0' ||
	    parsed < min || parsed > max)
		return -1;
	*value = (int)parsed;
	return 0;
}

void config_set_defaults(struct sv_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strncpy(cfg->interface, "eth0", sizeof(cfg->interface) - 1);
	cfg->phc_device_set = 0;
	cfg->enable_hw_timestamps = 0;
	cfg->vlan_id = -1;
	cfg->mode = SV_MODE_DIRECT;
	cfg->role = SV_ROLE_SUBSCRIBER;
	strncpy(cfg->collector_addr, "127.0.0.1",
		sizeof(cfg->collector_addr) - 1);
	cfg->collector_port = 9200;
	cfg->prometheus_port = 9100;
	cfg->prometheus_enabled = 1;
	cfg->histogram_max_us = SV_HISTOGRAM_MAX_US;
	cfg->batch_size = 256;
	cfg->cpu_affinity = -1;
	cfg->sched_fifo = 0;
	cfg->sched_priority = 0;
	cfg->live_histogram = 0;
	cfg->live_threshold_us = 250;
	cfg->warmup_seconds = 0;
	cfg->output_path_set = 0;
}

void config_print_usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Options:\n"
		"  -i, --interface NAME      Network interface (default: eth0)\n"
		"  -p, --phc-device PATH     PHC device path (auto-detected if unset)\n"
		"  -E, --enable-hw-timestamps Configure device RX timestamp filter to all\n"
		"  -v, --vlan-id ID          VLAN ID filter (default: accept all)\n"
		"  -m, --mode MODE           'direct' or 'split' (default: direct)\n"
		"  -c, --collector ADDR:PORT Collector address (default: 127.0.0.1:9200)\n"
		"  -P, --prometheus-port N   Prometheus port (default: 9100)\n"
		"  -N, --no-prometheus       Disable the Prometheus endpoint\n"
		"  -o, --output FILE         Write direct-mode samples as TSV\n"
		"  -H, --histogram-max N     Max histogram bucket in us (default: 35000)\n"
		"  -b, --batch-size N        Batch size for split mode (default: 256)\n"
		"  -a, --cpu-affinity N      CPU core to pin capture thread\n"
		"  -s, --sched-fifo PRIO     Use SCHED_FIFO with given priority\n"
		"  -L, --live-histogram      Show cumulative console histograms\n"
		"  -T, --live-threshold-us N Count values above N us (default: 250)\n"
		"  -w, --warmup-seconds N    Ignore measurements during startup (default: 0)\n"
		"  -h, --help                Show this help\n",
		progname);
}

int config_parse_args(struct sv_config *cfg, int argc, char **argv)
{
	static const struct option long_opts[] = {
		{"interface",      required_argument, NULL, 'i'},
		{"phc-device",     required_argument, NULL, 'p'},
		{"enable-hw-timestamps", no_argument, NULL, 'E'},
		{"vlan-id",        required_argument, NULL, 'v'},
		{"mode",           required_argument, NULL, 'm'},
		{"collector",      required_argument, NULL, 'c'},
		{"prometheus-port", required_argument, NULL, 'P'},
		{"no-prometheus",   no_argument,       NULL, 'N'},
		{"output",          required_argument, NULL, 'o'},
		{"histogram-max",  required_argument, NULL, 'H'},
		{"batch-size",     required_argument, NULL, 'b'},
		{"cpu-affinity",   required_argument, NULL, 'a'},
		{"sched-fifo",     required_argument, NULL, 's'},
		{"live-histogram", no_argument,       NULL, 'L'},
		{"live-threshold-us", required_argument, NULL, 'T'},
		{"warmup-seconds", required_argument, NULL, 'w'},
		{"help",           no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	/* Allow callers and unit tests to parse more than one argument vector. */
	optind = 1;
	int opt;
	while ((opt = getopt_long(argc, argv, "i:p:Ev:m:c:P:No:H:b:a:s:LT:w:h",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'i':
			strncpy(cfg->interface, optarg,
				sizeof(cfg->interface) - 1);
			break;
		case 'p':
			strncpy(cfg->phc_device, optarg,
				sizeof(cfg->phc_device) - 1);
			cfg->phc_device_set = 1;
			break;
		case 'E':
			cfg->enable_hw_timestamps = 1;
			break;
		case 'v':
			if (parse_int(optarg, -1, 4095, &cfg->vlan_id) < 0) {
				fprintf(stderr, "Invalid VLAN ID: %s\n", optarg);
				return -1;
			}
			break;
		case 'm':
			if (strcmp(optarg, "direct") == 0)
				cfg->mode = SV_MODE_DIRECT;
			else if (strcmp(optarg, "split") == 0)
				cfg->mode = SV_MODE_SPLIT;
			else {
				fprintf(stderr, "Invalid mode: %s\n", optarg);
				return -1;
			}
			break;
		case 'c': {
			char *colon = strrchr(optarg, ':');
			if (colon) {
				size_t addr_len = (size_t)(colon - optarg);
				int port;
				if (addr_len == 0 || addr_len >= sizeof(cfg->collector_addr) ||
				    parse_int(colon + 1, 1, 65535, &port) < 0) {
					fprintf(stderr, "Invalid collector address: %s\n", optarg);
					return -1;
				}
				memcpy(cfg->collector_addr, optarg, addr_len);
				cfg->collector_addr[addr_len] = '\0';
				cfg->collector_port = (uint16_t)port;
			} else {
				if (*optarg == '\0' || strlen(optarg) >= sizeof(cfg->collector_addr)) {
					fprintf(stderr, "Invalid collector address: %s\n", optarg);
					return -1;
				}
				strncpy(cfg->collector_addr, optarg,
					sizeof(cfg->collector_addr) - 1);
			}
			break;
		}
		case 'P': {
			int port;
			if (parse_int(optarg, 1, 65535, &port) < 0) {
				fprintf(stderr, "Invalid Prometheus port: %s\n", optarg);
				return -1;
			}
			cfg->prometheus_port = (uint16_t)port;
			break;
		}
		case 'N':
			cfg->prometheus_enabled = 0;
			break;
		case 'o':
			if (*optarg == '\0' || strlen(optarg) >= sizeof(cfg->output_path)) {
				fprintf(stderr, "Invalid output path: %s\n", optarg);
				return -1;
			}
			strncpy(cfg->output_path, optarg,
				sizeof(cfg->output_path) - 1);
			cfg->output_path_set = 1;
			break;
		case 'H':
			if (parse_int(optarg, 0, SV_HISTOGRAM_MAX_US,
				      &cfg->histogram_max_us) < 0) {
				fprintf(stderr, "Invalid histogram maximum: %s\n", optarg);
				return -1;
			}
			break;
		case 'b':
			if (parse_int(optarg, 1, 512, &cfg->batch_size) < 0) {
				fprintf(stderr, "Invalid batch size: %s\n", optarg);
				return -1;
			}
			break;
		case 'a':
			if (parse_int(optarg, 0, CPU_SETSIZE - 1,
				      &cfg->cpu_affinity) < 0) {
				fprintf(stderr, "Invalid CPU affinity: %s\n", optarg);
				return -1;
			}
			break;
		case 's':
			if (parse_int(optarg, 1, 99, &cfg->sched_priority) < 0) {
				fprintf(stderr, "Invalid SCHED_FIFO priority: %s\n", optarg);
				return -1;
			}
			cfg->sched_fifo = 1;
			break;
		case 'L':
			cfg->live_histogram = 1;
			break;
		case 'T':
			if (parse_int(optarg, 0, SV_HISTOGRAM_MAX_US,
				      &cfg->live_threshold_us) < 0) {
				fprintf(stderr, "Invalid live threshold: %s\n", optarg);
				return -1;
			}
			break;
		case 'w':
			if (parse_int(optarg, 0, 3600, &cfg->warmup_seconds) < 0) {
				fprintf(stderr, "Invalid warmup duration: %s\n", optarg);
				return -1;
			}
			break;
		case 'h':
			config_print_usage(argv[0]);
			return -1;
		default:
			config_print_usage(argv[0]);
			return -1;
		}
	}

	if (cfg->live_histogram &&
	    (cfg->role != SV_ROLE_SUBSCRIBER || cfg->mode != SV_MODE_DIRECT)) {
		fprintf(stderr,
			"Live histograms require sv-subscriber direct mode\n");
		return -1;
	}
	if (cfg->live_histogram &&
	    cfg->live_threshold_us > cfg->histogram_max_us) {
		fprintf(stderr,
			"Live threshold must not exceed histogram maximum\n");
		return -1;
	}
	if (cfg->output_path_set &&
	    (cfg->role != SV_ROLE_SUBSCRIBER || cfg->mode != SV_MODE_DIRECT)) {
		fprintf(stderr,
			"Output files require sv-subscriber direct mode\n");
		return -1;
	}
	if (cfg->warmup_seconds > 0 &&
	    (cfg->role != SV_ROLE_SUBSCRIBER || cfg->mode != SV_MODE_DIRECT)) {
		fprintf(stderr,
			"Warmup duration requires sv-subscriber direct mode\n");
		return -1;
	}

	return 0;
}
