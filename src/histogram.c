/* SPDX-License-Identifier: Apache-2.0 */
#include "histogram.h"
#include <string.h>

void histogram_init(struct sv_histogram *h)
{
	memset(h, 0, sizeof(*h));
}

void histogram_record(struct sv_histogram *h, int64_t latency_us)
{
	if (latency_us < 0)
		latency_us = 0;

	if (latency_us < SV_HISTOGRAM_BINS) {
		atomic_fetch_add_explicit(&h->buckets[latency_us], 1,
					 memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&h->overflow, 1,
					 memory_order_relaxed);
		latency_us = SV_HISTOGRAM_BINS; /* cap for sum */
	}

	atomic_fetch_add_explicit(&h->sum, (uint64_t)latency_us,
				 memory_order_relaxed);
	atomic_fetch_add_explicit(&h->count, 1, memory_order_relaxed);
}

void histogram_snapshot(const struct sv_histogram *h,
			uint64_t out_buckets[SV_HISTOGRAM_BINS],
			uint64_t *out_overflow,
			uint64_t *out_sum,
			uint64_t *out_count)
{
	for (int i = 0; i < SV_HISTOGRAM_BINS; i++)
		out_buckets[i] = atomic_load_explicit(&h->buckets[i],
						      memory_order_relaxed);
	*out_overflow = atomic_load_explicit(&h->overflow,
					     memory_order_relaxed);
	*out_sum = atomic_load_explicit(&h->sum, memory_order_relaxed);
	*out_count = atomic_load_explicit(&h->count, memory_order_relaxed);
}
