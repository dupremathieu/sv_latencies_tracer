/* SPDX-License-Identifier: Apache-2.0 */
#include "histogram.h"
#include <stdlib.h>
#include <string.h>

void histogram_init(struct sv_histogram *h)
{
	histogram_init_with_max(h, SV_HISTOGRAM_MAX_US);
}

void histogram_init_with_max(struct sv_histogram *h, int max_bucket_us)
{
	memset(h, 0, sizeof(*h));
	if (max_bucket_us < 0)
		max_bucket_us = 0;
	if (max_bucket_us > SV_HISTOGRAM_MAX_US)
		max_bucket_us = SV_HISTOGRAM_MAX_US;
	h->max_bucket_us = max_bucket_us;
	pthread_mutex_init(&h->exact_lock, NULL);
	h->buckets = calloc(SV_HISTOGRAM_BINS, sizeof(*h->buckets));
}

void histogram_destroy(struct sv_histogram *h)
{
	free(h->buckets);
	h->buckets = NULL;
	free(h->exact_overflow);
	h->exact_overflow = NULL;
	pthread_mutex_destroy(&h->exact_lock);
}

void histogram_record(struct sv_histogram *h, int64_t latency_us)
{
	if (latency_us < 0)
		latency_us = 0;

	uint64_t value = (uint64_t)latency_us;
	uint64_t current_max = atomic_load_explicit(&h->max,
						      memory_order_relaxed);
	while (value > current_max &&
	       !atomic_compare_exchange_weak_explicit(&h->max, &current_max,
						      value,
						      memory_order_relaxed,
						      memory_order_relaxed))
		;

	if (latency_us <= h->max_bucket_us) {
		atomic_fetch_add_explicit(&h->buckets[latency_us], 1,
						 memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&h->overflow, 1,
						 memory_order_relaxed);
		pthread_mutex_lock(&h->exact_lock);
		for (size_t i = 0; i < h->exact_overflow_count; i++) {
			if (h->exact_overflow[i].latency_us == value) {
				h->exact_overflow[i].count++;
				pthread_mutex_unlock(&h->exact_lock);
				goto recorded;
			}
		}
		if (h->exact_overflow_count == h->exact_overflow_capacity) {
			size_t new_capacity = h->exact_overflow_capacity == 0
				? 8 : h->exact_overflow_capacity * 2;
			struct sv_histogram_observation *observations =
				realloc(h->exact_overflow,
					new_capacity * sizeof(*observations));
			if (!observations) {
				pthread_mutex_unlock(&h->exact_lock);
				goto recorded;
			}
			h->exact_overflow = observations;
			h->exact_overflow_capacity = new_capacity;
		}
		h->exact_overflow[h->exact_overflow_count++] =
			(struct sv_histogram_observation){ value, 1 };
		pthread_mutex_unlock(&h->exact_lock);
	}


recorded:
	/* Keep the sum exact even when the value is outside the bucket range. */
	atomic_fetch_add_explicit(&h->sum, value,
					 memory_order_relaxed);
	atomic_fetch_add_explicit(&h->count, 1, memory_order_relaxed);
}

uint64_t histogram_max(const struct sv_histogram *h)
{
	return atomic_load_explicit(&h->max, memory_order_relaxed);
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

int histogram_snapshot_exact(const struct sv_histogram *h,
				     struct sv_histogram_observation **out,
				     size_t *out_count)
{
	struct sv_histogram *mutable_h = (struct sv_histogram *)h;
	*out = NULL;
	*out_count = 0;

	pthread_mutex_lock(&mutable_h->exact_lock);
	if (mutable_h->exact_overflow_count > 0) {
		*out = malloc(mutable_h->exact_overflow_count * sizeof(**out));
		if (!*out) {
			pthread_mutex_unlock(&mutable_h->exact_lock);
			return -1;
		}
		memcpy(*out, mutable_h->exact_overflow,
		       mutable_h->exact_overflow_count * sizeof(**out));
		*out_count = mutable_h->exact_overflow_count;
	}
	pthread_mutex_unlock(&mutable_h->exact_lock);
	return 0;
}
