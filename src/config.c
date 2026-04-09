/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_set_defaults(struct sv_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strncpy(cfg->interface, "eth0", sizeof(cfg->interface) - 1);
	cfg->phc_device_set = 0;
	cfg->vlan_id = -1;
	cfg->mode = SV_MODE_DIRECT;
	cfg->role = SV_ROLE_SUBSCRIBER;
	strncpy(cfg->collector_addr, "127.0.0.1",
		sizeof(cfg->collector_addr) - 1);
	cfg->collector_port = 9200;
	cfg->prometheus_port = 9100;
	cfg->histogram_max_us = 500;
	cfg->batch_size = 256;
	cfg->cpu_affinity = -1;
	cfg->sched_fifo = 0;
	cfg->sched_priority = 0;
}

void config_print_usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Options:\n"
		"  -i, --interface NAME      Network interface (default: eth0)\n"
		"  -p, --phc-device PATH     PHC device path (auto-detected if unset)\n"
		"  -v, --vlan-id ID          VLAN ID filter (default: accept all)\n"
		"  -m, --mode MODE           'direct' or 'split' (default: direct)\n"
		"  -c, --collector ADDR:PORT Collector address (default: 127.0.0.1:9200)\n"
		"  -P, --prometheus-port N   Prometheus port (default: 9100)\n"
		"  -H, --histogram-max N     Max histogram bucket in us (default: 500)\n"
		"  -b, --batch-size N        Batch size for split mode (default: 256)\n"
		"  -a, --cpu-affinity N      CPU core to pin capture thread\n"
		"  -s, --sched-fifo PRIO     Use SCHED_FIFO with given priority\n"
		"  -h, --help                Show this help\n",
		progname);
}

int config_parse_args(struct sv_config *cfg, int argc, char **argv)
{
	static const struct option long_opts[] = {
		{"interface",      required_argument, NULL, 'i'},
		{"phc-device",     required_argument, NULL, 'p'},
		{"vlan-id",        required_argument, NULL, 'v'},
		{"mode",           required_argument, NULL, 'm'},
		{"collector",      required_argument, NULL, 'c'},
		{"prometheus-port", required_argument, NULL, 'P'},
		{"histogram-max",  required_argument, NULL, 'H'},
		{"batch-size",     required_argument, NULL, 'b'},
		{"cpu-affinity",   required_argument, NULL, 'a'},
		{"sched-fifo",     required_argument, NULL, 's'},
		{"help",           no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:p:v:m:c:P:H:b:a:s:h",
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
		case 'v':
			cfg->vlan_id = atoi(optarg);
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
				if (addr_len >= sizeof(cfg->collector_addr))
					addr_len = sizeof(cfg->collector_addr) - 1;
				memcpy(cfg->collector_addr, optarg, addr_len);
				cfg->collector_addr[addr_len] = '\0';
				cfg->collector_port = (uint16_t)atoi(colon + 1);
			} else {
				strncpy(cfg->collector_addr, optarg,
					sizeof(cfg->collector_addr) - 1);
			}
			break;
		}
		case 'P':
			cfg->prometheus_port = (uint16_t)atoi(optarg);
			break;
		case 'H':
			cfg->histogram_max_us = atoi(optarg);
			break;
		case 'b':
			cfg->batch_size = atoi(optarg);
			break;
		case 'a':
			cfg->cpu_affinity = atoi(optarg);
			break;
		case 's':
			cfg->sched_fifo = 1;
			cfg->sched_priority = atoi(optarg);
			break;
		case 'h':
			config_print_usage(argv[0]);
			return -1;
		default:
			config_print_usage(argv[0]);
			return -1;
		}
	}

	return 0;
}
