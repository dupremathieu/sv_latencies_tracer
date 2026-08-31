/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_METRICS_H
#define SV_METRICS_H

#include "histogram.h"
#include "drop_detector.h"
#include <pthread.h>

/* Per-stream metric data */
struct sv_stream_metrics {
	struct sv_stream_id id;
	struct sv_histogram capture_latency;  /* T_app - T_rx */
	struct sv_histogram parsed_latency;   /* T_parsed - T_rx */
	struct sv_histogram hw_to_app_latency; /* PHC app time - HW RX TS */
	struct sv_histogram sw_to_app_latency; /* realtime app time - SW RX TS */
	struct sv_histogram hw_to_sw_latency;  /* estimated driver RX duration */
	struct sv_histogram interval_hw;      /* inter-sample, selected RX TS */
	struct sv_histogram interval_app;     /* inter-sample, app TS */
	_Atomic int64_t interval_hw_current_ns; /* latest inter-frame interval */
	_Atomic int64_t interval_app_current_ns;
	_Atomic uint64_t timestamp_hardware_total;
	_Atomic uint64_t timestamp_software_total;
	_Atomic uint64_t timestamp_application_total;
	int active;

	/* Interval bookkeeping (guarded by metrics_state.interval_lock) */
	uint16_t last_smp_cnt;
	struct sv_timestamp last_rx_ts;
	struct sv_timestamp last_app_ts;
	enum sv_timestamp_source last_timestamp_source;
	int have_prev;
};

struct sv_metrics_state {
	struct sv_stream_metrics streams[SV_MAX_STREAMS];
	int num_streams;
	int histogram_max_us;
	pthread_mutex_t stream_lock;
	pthread_mutex_t interval_lock;

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

void metrics_init_with_max(struct sv_metrics_state *ms, int histogram_max_us);

/*
 * Record the interval between two successively received SV frames of the same
 * stream. The first frame initializes the timestamps without recording a value.
 */
int metrics_record_interval(struct sv_metrics_state *ms, uint16_t app_id,
			    const char *sv_id, uint16_t smp_cnt,
			    const struct sv_timestamp *rx_ts,
			    const struct sv_timestamp *app_ts,
			    enum sv_timestamp_source source);

void metrics_record_timestamp_source(struct sv_stream_metrics *stream,
				     enum sv_timestamp_source source);

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
