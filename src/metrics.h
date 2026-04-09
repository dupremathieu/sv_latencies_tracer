/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_METRICS_H
#define SV_METRICS_H

#include "histogram.h"
#include "drop_detector.h"

/* Per-stream metric data */
struct sv_stream_metrics {
	struct sv_stream_id id;
	struct sv_histogram capture_latency;  /* T_app - T_hw */
	struct sv_histogram parsed_latency;   /* T_parsed - T_hw */
	int active;
};

struct sv_metrics_state {
	struct sv_stream_metrics streams[SV_MAX_STREAMS];
	int num_streams;

	/* System monitor counters */
	_Atomic uint64_t kernel_oops_total;
	_Atomic uint64_t kernel_panic_total;
	_Atomic uint64_t rt_throttle_total;
	_Atomic int      link_up;
};

/*
 * Find or create a stream metrics entry. Returns NULL if table is full.
 */
struct sv_stream_metrics *metrics_get_stream(struct sv_metrics_state *ms,
					     uint16_t app_id,
					     const char *sv_id);

void metrics_init(struct sv_metrics_state *ms);

/*
 * Format all metrics in Prometheus text exposition format.
 * Returns a malloc'd string (caller frees), or NULL on error.
 */
char *metrics_format(const struct sv_metrics_state *ms,
		     const struct sv_drop_tracker *dt);

/*
 * Start the HTTP metrics server on the given port.
 * This spawns a background thread. Returns 0 on success.
 */
int metrics_server_start(uint16_t port, struct sv_metrics_state *ms,
			 struct sv_drop_tracker *dt);

void metrics_server_stop(void);

#endif /* SV_METRICS_H */
