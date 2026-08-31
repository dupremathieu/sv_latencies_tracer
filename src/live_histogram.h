/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_LIVE_HISTOGRAM_H
#define SV_LIVE_HISTOGRAM_H

#include "metrics.h"

struct sv_live_histogram_ctx {
	struct sv_metrics_state *metrics;
	int threshold_us;
	_Atomic int running;
	pthread_t thread;
	int started;
};

int live_histogram_start(struct sv_live_histogram_ctx *ctx,
			 struct sv_metrics_state *metrics, int threshold_us);

void live_histogram_stop(struct sv_live_histogram_ctx *ctx);

#endif /* SV_LIVE_HISTOGRAM_H */
