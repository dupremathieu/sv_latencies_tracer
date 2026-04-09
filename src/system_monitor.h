/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_SYSTEM_MONITOR_H
#define SV_SYSTEM_MONITOR_H

#include <pthread.h>
#include "metrics.h"

struct sv_sysmon_ctx {
	struct sv_metrics_state *ms;
	const char *ifname;
	pthread_t link_thread;
	pthread_t kmsg_thread;
	pthread_t rt_thread;
	volatile int running;
};

/*
 * Start all system monitor threads (link state, kmsg, RT throttle).
 * Returns 0 on success.
 */
int sysmon_start(struct sv_sysmon_ctx *ctx, struct sv_metrics_state *ms,
		 const char *ifname);

void sysmon_stop(struct sv_sysmon_ctx *ctx);

#endif /* SV_SYSTEM_MONITOR_H */
