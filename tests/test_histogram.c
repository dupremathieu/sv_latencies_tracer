/* SPDX-License-Identifier: Apache-2.0 */
#include "histogram.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void test_basic_record(void)
{
	printf("  test_basic_record...");
	struct sv_histogram h;
	histogram_init(&h);

	histogram_record(&h, 100);
	histogram_record(&h, 100);
	histogram_record(&h, 200);

	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	assert(buckets != NULL);
	uint64_t overflow, sum, count;
	histogram_snapshot(&h, buckets, &overflow, &sum, &count);

	assert(buckets[100] == 2);
	assert(buckets[200] == 1);
	assert(overflow == 0);
	assert(sum == 400);
	assert(count == 3);
	assert(histogram_max(&h) == 200);
	free(buckets);
	histogram_destroy(&h);
	printf(" OK\n");
}

static void test_overflow(void)
{
	printf("  test_overflow...");
	struct sv_histogram h;
	histogram_init(&h);

	histogram_record(&h, SV_HISTOGRAM_MAX_US + 1);

	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	assert(buckets != NULL);
	uint64_t overflow, sum, count;
	histogram_snapshot(&h, buckets, &overflow, &sum, &count);

	assert(overflow == 1);
	assert(count == 1);
	assert(histogram_max(&h) == SV_HISTOGRAM_MAX_US + 1);
	assert(sum == SV_HISTOGRAM_MAX_US + 1);
	free(buckets);
	histogram_destroy(&h);
	printf(" OK\n");
}

static void test_negative_clamped(void)
{
	printf("  test_negative_clamped...");
	struct sv_histogram h;
	histogram_init(&h);

	histogram_record(&h, -5);

	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	assert(buckets != NULL);
	uint64_t overflow, sum, count;
	histogram_snapshot(&h, buckets, &overflow, &sum, &count);

	assert(buckets[0] == 1);
	assert(count == 1);
	free(buckets);
	histogram_destroy(&h);
	printf(" OK\n");
}

static void test_boundary(void)
{
	printf("  test_boundary...");
	struct sv_histogram h;
	histogram_init(&h);

	histogram_record(&h, 0);
	histogram_record(&h, 500);
	histogram_record(&h, SV_HISTOGRAM_MAX_US + 1);

	uint64_t *buckets = malloc(sizeof(*buckets) * SV_HISTOGRAM_BINS);
	assert(buckets != NULL);
	uint64_t overflow, sum, count;
	histogram_snapshot(&h, buckets, &overflow, &sum, &count);

	assert(buckets[0] == 1);
	assert(buckets[500] == 1);
	assert(overflow == 1);
	assert(sum == SV_HISTOGRAM_MAX_US + 501);
	assert(count == 3);
	free(buckets);
	histogram_destroy(&h);
	printf(" OK\n");
}

int main(void)
{
	printf("=== Histogram Tests ===\n");
	test_basic_record();
	test_overflow();
	test_negative_clamped();
	test_boundary();
	printf("All histogram tests passed.\n");
	return 0;
}
