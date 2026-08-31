/* SPDX-License-Identifier: Apache-2.0 */
#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_live_histogram_options(void)
{
	struct sv_config cfg;
	char *argv[] = {
		"sv-subscriber",
		"--live-histogram",
		"--live-threshold-us",
		"209",
	};

	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_SUBSCRIBER;
	assert(cfg.live_histogram == 0);
	assert(cfg.live_threshold_us == 250);
	assert(config_parse_args(&cfg, 4, argv) == 0);
	assert(cfg.live_histogram == 1);
	assert(cfg.live_threshold_us == 209);
}

static void test_output_options(void)
{
	struct sv_config cfg;
	char *argv[] = {
		"sv-subscriber",
		"--output", "/tmp/results.tsv",
		"--no-prometheus",
	};

	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_SUBSCRIBER;
	assert(cfg.prometheus_enabled == 1);
	assert(config_parse_args(&cfg, 4, argv) == 0);
	assert(cfg.output_path_set == 1);
	assert(strcmp(cfg.output_path, "/tmp/results.tsv") == 0);
	assert(cfg.prometheus_enabled == 0);
}

static void test_warmup_options(void)
{
	struct sv_config cfg;
	char *argv[] = {
		"sv-subscriber",
		"--warmup-seconds", "1",
	};

	config_set_defaults(&cfg);
	cfg.role = SV_ROLE_SUBSCRIBER;
	assert(cfg.warmup_seconds == 0);
	assert(config_parse_args(&cfg, 3, argv) == 0);
	assert(cfg.warmup_seconds == 1);
}

static void test_enable_hw_timestamp_option(void)
{
	struct sv_config cfg;
	char *argv[] = {
		"sv-subscriber",
		"--enable-hw-timestamps",
	};

	config_set_defaults(&cfg);
	assert(cfg.enable_hw_timestamps == 0);
	assert(config_parse_args(&cfg, 2, argv) == 0);
	assert(cfg.enable_hw_timestamps == 1);
}

int main(void)
{
	printf("=== Config Tests ===\n");
	test_live_histogram_options();
	test_output_options();
	test_warmup_options();
	test_enable_hw_timestamp_option();
	printf("All config tests passed.\n");
	return 0;
}
