/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_HISTOGRAM_H
#define SV_HISTOGRAM_H

#include "common.h"

struct sv_histogram {
	_Atomic uint64_t buckets[SV_HISTOGRAM_BINS];
	_Atomic uint64_t overflow;
	_Atomic uint64_t sum;    /* cumulative sum in µs for Prometheus */
	_Atomic uint64_t count;  /* total observations */
};

void histogram_init(struct sv_histogram *h);

/* Record a latency value in microseconds. */
void histogram_record(struct sv_histogram *h, int64_t latency_us);

/* Snapshot the histogram into a plain (non-atomic) output array for reading. */
void histogram_snapshot(const struct sv_histogram *h,
			uint64_t out_buckets[SV_HISTOGRAM_BINS],
			uint64_t *out_overflow,
			uint64_t *out_sum,
			uint64_t *out_count);

#endif /* SV_HISTOGRAM_H */
