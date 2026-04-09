/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_CONFIG_H
#define SV_CONFIG_H

#include <stdint.h>

enum sv_mode {
	SV_MODE_DIRECT,  /* Scenario A */
	SV_MODE_SPLIT,   /* Scenario B */
};

enum sv_role {
	SV_ROLE_SUBSCRIBER,
	SV_ROLE_CAPTURE_AGENT,
	SV_ROLE_COLLECTOR,
};

struct sv_config {
	char     interface[64];
	char     phc_device[64];
	int      phc_device_set;
	int      vlan_id;         /* -1 = accept all */
	enum sv_mode mode;
	enum sv_role role;
	char     collector_addr[128];
	uint16_t collector_port;
	uint16_t prometheus_port;
	int      histogram_max_us;
	int      batch_size;
	int      cpu_affinity;    /* -1 = unset */
	int      sched_fifo;
	int      sched_priority;
};

void config_set_defaults(struct sv_config *cfg);

/*
 * Parse command-line arguments.
 * Returns 0 on success, -1 on error (with usage printed to stderr).
 */
int config_parse_args(struct sv_config *cfg, int argc, char **argv);

void config_print_usage(const char *progname);

#endif /* SV_CONFIG_H */
