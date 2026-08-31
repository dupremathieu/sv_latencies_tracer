/* SPDX-License-Identifier: Apache-2.0 */
#include "live_histogram.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIVE_CHART_WIDTH 32

struct live_stats {
	uint64_t count;
	uint64_t min;
	uint64_t p50;
	uint64_t p99;
	uint64_t max;
	uint64_t above_threshold;
	char chart[LIVE_CHART_WIDTH + 1];
};

static void build_chart(const uint64_t *delta, int max_bucket,
			uint64_t overflow, struct live_stats *stats)
{
	static const char levels[] = " .:-=+*#@";
	uint64_t columns[LIVE_CHART_WIDTH] = { 0 };
	uint64_t lower = stats->min;
	uint64_t upper = stats->p99;

	if (lower > (uint64_t)max_bucket)
		lower = (uint64_t)max_bucket;
	if (upper > (uint64_t)max_bucket)
		upper = (uint64_t)max_bucket;
	if (upper < lower)
		upper = lower;

	for (int i = 0; i <= max_bucket; i++) {
		if (delta[i] == 0)
			continue;

		int column;
		if (upper == lower) {
			column = LIVE_CHART_WIDTH / 2;
		} else if ((uint64_t)i <= lower) {
			column = 0;
		} else if ((uint64_t)i >= upper) {
			column = LIVE_CHART_WIDTH - 1;
		} else {
			column = (int)(((uint64_t)i - lower) *
				       (LIVE_CHART_WIDTH - 1) /
				       (upper - lower));
		}
		columns[column] += delta[i];
	}
	columns[LIVE_CHART_WIDTH - 1] += overflow;

	uint64_t largest = 0;
	for (int i = 0; i < LIVE_CHART_WIDTH; i++) {
		if (columns[i] > largest)
			largest = columns[i];
	}

	for (int i = 0; i < LIVE_CHART_WIDTH; i++) {
		if (columns[i] == 0) {
			stats->chart[i] = levels[0];
			continue;
		}
		size_t level = 1 + (size_t)(columns[i] *
					    (sizeof(levels) - 3) / largest);
		stats->chart[i] = levels[level];
	}
	stats->chart[LIVE_CHART_WIDTH] = '\0';
}

static void snapshot_cumulative(const struct sv_histogram *histogram,
				uint64_t *snapshot, int threshold_us,
				struct live_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->min = (uint64_t)histogram->max_bucket_us + 1;

	for (int i = 0; i <= histogram->max_bucket_us; i++) {
		snapshot[i] = atomic_load_explicit(&histogram->buckets[i],
						   memory_order_relaxed);
		stats->count += snapshot[i];
		if (snapshot[i] > 0) {
			if ((uint64_t)i < stats->min)
				stats->min = (uint64_t)i;
			stats->max = (uint64_t)i;
			if (i > threshold_us)
				stats->above_threshold += snapshot[i];
		}
	}

	uint64_t overflow = atomic_load_explicit(&histogram->overflow,
						 memory_order_relaxed);
	stats->count += overflow;
	stats->above_threshold += overflow;

	if (stats->count == 0) {
		memset(stats->chart, ' ', LIVE_CHART_WIDTH);
		stats->chart[LIVE_CHART_WIDTH] = '\0';
		return;
	}

	uint64_t p50_rank = (stats->count + 1) / 2;
	uint64_t p99_rank = (stats->count * 99 + 99) / 100;
	uint64_t cumulative = 0;
	int p50_found = 0;
	int p99_found = 0;
	for (int i = 0; i <= histogram->max_bucket_us; i++) {
		cumulative += snapshot[i];
		if (!p50_found && cumulative >= p50_rank) {
			stats->p50 = (uint64_t)i;
			p50_found = 1;
		}
		if (!p99_found && cumulative >= p99_rank) {
			stats->p99 = (uint64_t)i;
			p99_found = 1;
			break;
		}
	}

	uint64_t overflow_value = (uint64_t)histogram->max_bucket_us + 1;
	if (stats->min == overflow_value)
		stats->min = overflow_value;
	if (!p50_found)
		stats->p50 = overflow_value;
	if (!p99_found)
		stats->p99 = overflow_value;
	stats->max = histogram_max(histogram);

	build_chart(snapshot, histogram->max_bucket_us, overflow, stats);
}

static void format_value(char *buffer, size_t size, uint64_t value,
			 int max_bucket)
{
	if (value > (uint64_t)max_bucket)
		snprintf(buffer, size, ">%d", max_bucket);
	else
		snprintf(buffer, size, "%lu", (unsigned long)value);
}

static void print_stats(const char *label, const struct live_stats *stats,
			int threshold_us, int max_bucket)
{
	if (stats->count == 0) {
		fprintf(stderr, "  %-20s n=0 (waiting)\n", label);
		return;
	}

	char min[24], p50[24], p99[24], max[24];
	format_value(min, sizeof(min), stats->min, max_bucket);
	format_value(p50, sizeof(p50), stats->p50, max_bucket);
	format_value(p99, sizeof(p99), stats->p99, max_bucket);
	snprintf(max, sizeof(max), "%lu", (unsigned long)stats->max);
	double percent = 100.0 * (double)stats->above_threshold /
			 (double)stats->count;

	fprintf(stderr,
		"  %-20s n=%-6lu min=%-6s p50=%-6s p99=%-6s max=%-6s "
		">%dus=%lu (%.1f%%)\n",
		label, (unsigned long)stats->count, min, p50, p99, max,
		threshold_us, (unsigned long)stats->above_threshold, percent);
	fprintf(stderr, "    [%s] range min..p99 us\n", stats->chart);
}

static void render_live_histograms(struct sv_live_histogram_ctx *ctx,
				   uint64_t *snapshot)
{
	struct sv_stream_metrics *streams[SV_MAX_STREAMS];
	int num_streams = 0;

	pthread_mutex_lock(&ctx->metrics->stream_lock);
	for (int i = 0; i < ctx->metrics->num_streams; i++) {
		if (ctx->metrics->streams[i].active) {
			streams[num_streams] = &ctx->metrics->streams[i];
			num_streams++;
		}
	}
	pthread_mutex_unlock(&ctx->metrics->stream_lock);

	flockfile(stderr);
	if (isatty(STDERR_FILENO))
		fputs("\033[H\033[2J", stderr);
	fprintf(stderr, "SV live | since listening started | threshold >%dus\n",
		ctx->threshold_us);

	if (num_streams == 0)
		fputs("Waiting for SV frames...\n", stderr);

	for (int i = 0; i < num_streams; i++) {
		struct sv_stream_metrics *stream = streams[i];
		struct live_stats capture;
		struct live_stats hw_to_app;
		struct live_stats sw_to_app;
		struct live_stats hw_to_sw;
		struct live_stats interval;
		snapshot_cumulative(&stream->capture_latency, snapshot,
				    ctx->threshold_us, &capture);
		snapshot_cumulative(&stream->hw_to_app_latency, snapshot,
				    ctx->threshold_us, &hw_to_app);
		snapshot_cumulative(&stream->sw_to_app_latency, snapshot,
				    ctx->threshold_us, &sw_to_app);
		snapshot_cumulative(&stream->hw_to_sw_latency, snapshot,
				    ctx->threshold_us, &hw_to_sw);
		snapshot_cumulative(&stream->interval_app, snapshot,
				    ctx->threshold_us, &interval);

		fprintf(stderr, "Stream appid=0x%04X svid=%s\n",
			stream->id.app_id, stream->id.sv_id);
		fprintf(stderr,
			"  Timestamp source     hardware=%lu software=%lu application=%lu\n",
			(unsigned long)atomic_load_explicit(
				&stream->timestamp_hardware_total, memory_order_relaxed),
			(unsigned long)atomic_load_explicit(
				&stream->timestamp_software_total, memory_order_relaxed),
			(unsigned long)atomic_load_explicit(
				&stream->timestamp_application_total, memory_order_relaxed));
		print_stats("RX TS -> application", &capture, ctx->threshold_us,
			    stream->capture_latency.max_bucket_us);
		print_stats("HW TS -> application", &hw_to_app,
			    ctx->threshold_us,
			    stream->hw_to_app_latency.max_bucket_us);
		print_stats("SW TS -> application", &sw_to_app,
			    ctx->threshold_us,
			    stream->sw_to_app_latency.max_bucket_us);
		print_stats("Estimated HW -> SW", &hw_to_sw,
			    ctx->threshold_us,
			    stream->hw_to_sw_latency.max_bucket_us);
		print_stats("Application interval", &interval,
			    ctx->threshold_us,
			    stream->interval_app.max_bucket_us);
	}
	funlockfile(stderr);
	fflush(stderr);
}

static void *live_histogram_thread(void *arg)
{
	struct sv_live_histogram_ctx *ctx = arg;
	uint64_t *snapshot = calloc(SV_HISTOGRAM_BINS, sizeof(*snapshot));
	struct timespec next;

	pthread_setname_np(pthread_self(), "sv-live-hist");
	if (!snapshot)
		return NULL;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (atomic_load_explicit(&ctx->running, memory_order_relaxed)) {
		next.tv_sec++;
		int ret;
		do {
			ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
					      &next, NULL);
		} while (ret == EINTR &&
			 atomic_load_explicit(&ctx->running, memory_order_relaxed));
		if (!atomic_load_explicit(&ctx->running, memory_order_relaxed))
			break;
		render_live_histograms(ctx, snapshot);
	}

	free(snapshot);
	return NULL;
}

int live_histogram_start(struct sv_live_histogram_ctx *ctx,
			 struct sv_metrics_state *metrics, int threshold_us)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->metrics = metrics;
	ctx->threshold_us = threshold_us;
	atomic_store_explicit(&ctx->running, 1, memory_order_relaxed);
	if (pthread_create(&ctx->thread, NULL, live_histogram_thread, ctx) != 0) {
		perror("live histogram: pthread_create");
		atomic_store_explicit(&ctx->running, 0, memory_order_relaxed);
		return -1;
	}
	ctx->started = 1;
	return 0;
}

void live_histogram_stop(struct sv_live_histogram_ctx *ctx)
{
	atomic_store_explicit(&ctx->running, 0, memory_order_relaxed);
	if (ctx->started) {
		pthread_join(ctx->thread, NULL);
		ctx->started = 0;
	}
}
