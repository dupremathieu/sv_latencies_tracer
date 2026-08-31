/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_HISTOGRAM_H
#define SV_HISTOGRAM_H

#include "common.h"
#include <stddef.h>
#include <pthread.h>

struct sv_histogram_observation {
	uint64_t latency_us;
	uint64_t count;
};

struct sv_histogram {
	_Atomic uint64_t *buckets;
	int max_bucket_us;
	_Atomic uint64_t overflow;
	_Atomic uint64_t sum;    /* cumulative sum in µs for Prometheus */
	_Atomic uint64_t count;  /* total observations */
	_Atomic uint64_t max;    /* largest uncapped observation in µs */
	pthread_mutex_t exact_lock;
	struct sv_histogram_observation *exact_overflow;
	size_t exact_overflow_count;
	size_t exact_overflow_capacity;
};

void histogram_init(struct sv_histogram *h);

void histogram_init_with_max(struct sv_histogram *h, int max_bucket_us);

void histogram_destroy(struct sv_histogram *h);

/* Record a latency value in microseconds. */
void histogram_record(struct sv_histogram *h, int64_t latency_us);

uint64_t histogram_max(const struct sv_histogram *h);

/* Snapshot the histogram into a plain (non-atomic) output array for reading. */
void histogram_snapshot(const struct sv_histogram *h,
			uint64_t out_buckets[SV_HISTOGRAM_BINS],
			uint64_t *out_overflow,
			uint64_t *out_sum,
			uint64_t *out_count);

/* Snapshot exact counts for observations above the configured bucket limit. */
int histogram_snapshot_exact(const struct sv_histogram *h,
				     struct sv_histogram_observation **out,
				     size_t *out_count);

#endif /* SV_HISTOGRAM_H */
