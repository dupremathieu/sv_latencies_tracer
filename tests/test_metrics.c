/* SPDX-License-Identifier: Apache-2.0 */
#include "metrics.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct sv_timestamp timestamp_us(uint64_t us)
{
	return (struct sv_timestamp){
		.sec = us / 1000000,
		.nsec = (uint32_t)(us % 1000000) * 1000,
	};
}

static void test_intervals_count_every_frame_pair(void)
{
	struct sv_metrics_state metrics;
	metrics_init(&metrics);

	struct sv_timestamp hw = timestamp_us(1000);
	struct sv_timestamp app = timestamp_us(1010);
	assert(metrics_record_interval(&metrics, 0x4000, "S1", 10,
				       &hw, &app,
				       SV_TIMESTAMP_SOURCE_SOFTWARE) == 0);

	hw = timestamp_us(1250);
	app = timestamp_us(1265);
	assert(metrics_record_interval(&metrics, 0x4000, "S1", 12,
				       &hw, &app,
				       SV_TIMESTAMP_SOURCE_SOFTWARE) == 0);

	hw = timestamp_us(1490);
	app = timestamp_us(1510);
	assert(metrics_record_interval(&metrics, 0x4000, "S1", 0,
				       &hw, &app,
				       SV_TIMESTAMP_SOURCE_SOFTWARE) == 0);

	struct sv_stream_metrics *stream =
		metrics_get_stream(&metrics, 0x4000, "S1");
	assert(stream != NULL);
	histogram_record(&stream->capture_latency, 23);
	histogram_record(&stream->capture_latency, 728);
	histogram_record(&stream->hw_to_app_latency, 245);
	histogram_record(&stream->sw_to_app_latency, 8);
	histogram_record(&stream->hw_to_sw_latency, 237);
	metrics_record_timestamp_source(stream, SV_TIMESTAMP_SOURCE_SOFTWARE);
	metrics_record_timestamp_source(stream, SV_TIMESTAMP_SOURCE_SOFTWARE);
	assert(atomic_load(&stream->interval_hw.count) == 2);
	assert(atomic_load(&stream->interval_app.count) == 2);
	assert(atomic_load(&stream->interval_hw_current_ns) == 240000);
	assert(atomic_load(&stream->interval_app_current_ns) == 245000);

	char *output = metrics_format(&metrics, NULL);
	assert(output != NULL);
	assert(strstr(output,
		      "sv_sv_interval_hw_us_count{appid=\"0x4000\",svid=\"S1\"} 2") != NULL);
	assert(strstr(output,
		      "sv_sv_interval_hw_current_us{appid=\"0x4000\",svid=\"S1\"} 240.000") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"23\"} 1") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"728\"} 1") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_bucket{appid=\"0x4000\",svid=\"S1\",le=\"35000\"}") == NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"727\"}") == NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_max{appid=\"0x4000\",svid=\"S1\"} 728") != NULL);
	assert(strstr(output,
		      "sv_hw_timestamp_to_app_latency_us_count{appid=\"0x4000\",svid=\"S1\"} 1") != NULL);
	assert(strstr(output,
		      "sv_sw_timestamp_to_app_latency_us_sum{appid=\"0x4000\",svid=\"S1\"} 8") != NULL);
	assert(strstr(output,
		      "sv_hw_to_sw_estimated_latency_us_bucket{appid=\"0x4000\",svid=\"S1\",le=\"237\"} 1") != NULL);
	assert(strstr(output,
		      "sv_timestamp_source_frames_total{appid=\"0x4000\",svid=\"S1\",source=\"hardware\"} 0") != NULL);
	assert(strstr(output,
		      "sv_timestamp_source_frames_total{appid=\"0x4000\",svid=\"S1\",source=\"software\"} 2") != NULL);
	free(output);
}

static void test_interval_resets_when_timestamp_source_changes(void)
{
	struct sv_metrics_state metrics;
	struct sv_timestamp rx = timestamp_us(1000);
	struct sv_timestamp app = timestamp_us(1010);

	metrics_init(&metrics);
	assert(metrics_record_interval(&metrics, 0x4000, "S1", 1,
				       &rx, &app,
				       SV_TIMESTAMP_SOURCE_HARDWARE) == 0);
	rx = timestamp_us(200000);
	app = timestamp_us(200010);
	assert(metrics_record_interval(&metrics, 0x4000, "S1", 2,
				       &rx, &app,
				       SV_TIMESTAMP_SOURCE_SOFTWARE) == 0);

	struct sv_stream_metrics *stream =
		metrics_get_stream(&metrics, 0x4000, "S1");
	assert(stream != NULL);
	assert(atomic_load(&stream->interval_hw.count) == 0);
	assert(atomic_load(&stream->interval_app.count) == 0);
}

static void test_observation_counts_are_exact(void)
{
	struct sv_metrics_state metrics;
	metrics_init(&metrics);
	struct sv_stream_metrics *stream =
		metrics_get_stream(&metrics, 0x4000, "S1");
	assert(stream != NULL);

	histogram_record(&stream->capture_latency, 100);
	histogram_record(&stream->capture_latency, 500);

	char *output = metrics_format(&metrics, NULL);
	assert(output != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"100\"} 1") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"500\"} 1") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"S1\",latency_us=\"101\"") == NULL);
	free(output);
}

static void test_observation_counts_include_large_values(void)
{
	struct sv_metrics_state metrics;
	metrics_init(&metrics);
	struct sv_stream_metrics *low =
		metrics_get_stream(&metrics, 0x4000, "LOW");
	struct sv_stream_metrics *high =
		metrics_get_stream(&metrics, 0x4000, "HIGH");
	assert(low != NULL);
	assert(high != NULL);

	histogram_record(&low->capture_latency, 23);
	histogram_record(&high->capture_latency, 300);

	char *output = metrics_format(&metrics, NULL);
	assert(output != NULL);
	assert(strstr(output,
		      "svid=\"LOW\",latency_us=\"23\"} 1") != NULL);
	assert(strstr(output,
		      "svid=\"HIGH\",latency_us=\"300\"} 1") != NULL);
	histogram_record(&low->capture_latency, SV_HISTOGRAM_MAX_US + 1);
	histogram_record(&low->capture_latency, SV_HISTOGRAM_MAX_US + 1);
	histogram_record(&low->capture_latency, 42000);
	free(output);

	output = metrics_format(&metrics, NULL);
	assert(output != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"LOW\",latency_us=\"35001\"} 2") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_observations_total{appid=\"0x4000\",svid=\"LOW\",latency_us=\"42000\"} 1") != NULL);
	assert(strstr(output,
		      "sv_capture_latency_us_max{appid=\"0x4000\",svid=\"LOW\"} 42000") != NULL);
 free(output);
}

int main(void)
{
	printf("=== Metrics Tests ===\n");
	test_intervals_count_every_frame_pair();
	test_interval_resets_when_timestamp_source_changes();
	test_observation_counts_are_exact();
	test_observation_counts_include_large_values();
	printf("All metrics tests passed.\n");
	return 0;
}
