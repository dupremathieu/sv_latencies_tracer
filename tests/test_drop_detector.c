/* SPDX-License-Identifier: Apache-2.0 */
#include "drop_detector.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct sv_frame_info make_info(uint16_t app_id, const char *sv_id,
				      uint16_t smp_cnt)
{
	struct sv_frame_info info = { .app_id = app_id, .smp_cnt = smp_cnt };
	sv_copy_svid(info.sv_id, sv_id);
	return info;
}

static void test_no_drops(void)
{
	printf("  test_no_drops...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 0);
	assert(drop_tracker_process(&dt, &info) == 0); /* first frame */

	info.smp_cnt = 1;
	assert(drop_tracker_process(&dt, &info) == 0);
	info.smp_cnt = 2;
	assert(drop_tracker_process(&dt, &info) == 0);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(s != NULL);
	assert(atomic_load(&s->frames_received) == 3);
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_single_drop(void)
{
	printf("  test_single_drop...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 10);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 12; /* skipped 11 */
	int gap = drop_tracker_process(&dt, &info);
	assert(gap == 1);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(atomic_load(&s->frames_dropped) == 1);
	printf(" OK\n");
}

static void test_multi_drop(void)
{
	printf("  test_multi_drop...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 100);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 105; /* skipped 101-104 */
	int gap = drop_tracker_process(&dt, &info);
	assert(gap == 4);
	printf(" OK\n");
}

static void test_wrap_around(void)
{
	printf("  test_wrap_around...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 3998);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 3999;
	assert(drop_tracker_process(&dt, &info) == 0);

	info.smp_cnt = 0; /* wrap */
	assert(drop_tracker_process(&dt, &info) == 0);

	info.smp_cnt = 1;
	assert(drop_tracker_process(&dt, &info) == 0);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_wrap_with_gap(void)
{
	printf("  test_wrap_with_gap...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 3998);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 1; /* skipped 3999 and 0 */
	int gap = drop_tracker_process(&dt, &info);
	assert(gap == 2);
	printf(" OK\n");
}

static void test_duplicate_ignored(void)
{
	printf("  test_duplicate_ignored...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 100);
	drop_tracker_process(&dt, &info);
	assert(drop_tracker_process(&dt, &info) == 0);

	info.smp_cnt = 101;
	assert(drop_tracker_process(&dt, &info) == 0);
	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_late_frame_ignored(void)
{
	printf("  test_late_frame_ignored...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 100);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 99;
	assert(drop_tracker_process(&dt, &info) == 0);
	info.smp_cnt = 101;
	assert(drop_tracker_process(&dt, &info) == 0);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_reset_to_zero(void)
{
	printf("  test_reset_to_zero...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 100);
	drop_tracker_process(&dt, &info);

	info.smp_cnt = 0; /* producer restart */
	assert(drop_tracker_process(&dt, &info) == 0);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(s != NULL);
	assert(s->last_smp_cnt == 0);
	assert(atomic_load(&s->frames_dropped) == 0);

	info.smp_cnt = 1;
	assert(drop_tracker_process(&dt, &info) == 0);
	assert(s->last_smp_cnt == 1);
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_idle_stream_restart(void)
{
	printf("  test_idle_stream_restart...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info info = make_info(0x4000, "S1", 2500);
	struct sv_timestamp ts = { .sec = 10, .nsec = 0 };
	drop_tracker_process_at(&dt, &info, &ts);

	info.smp_cnt = 1245;
	ts.sec = 12;
	assert(drop_tracker_process_at(&dt, &info, &ts) == 0);
	info.smp_cnt = 1246;
	ts.nsec = 250000;
	assert(drop_tracker_process_at(&dt, &info, &ts) == 0);

	const struct sv_drop_state *s = drop_tracker_find(&dt, 0x4000, "S1");
	assert(atomic_load(&s->frames_dropped) == 0);
	printf(" OK\n");
}

static void test_multiple_streams(void)
{
	printf("  test_multiple_streams...");
	struct sv_drop_tracker dt;
	drop_tracker_init(&dt);

	struct sv_frame_info i1 = make_info(0x4000, "S1", 0);
	struct sv_frame_info i2 = make_info(0x4001, "S2", 100);
	drop_tracker_process(&dt, &i1);
	drop_tracker_process(&dt, &i2);

	i1.smp_cnt = 1;
	assert(drop_tracker_process(&dt, &i1) == 0);
	i2.smp_cnt = 102;
	assert(drop_tracker_process(&dt, &i2) == 1);

	assert(dt.num_streams == 2);
	printf(" OK\n");
}

int main(void)
{
	printf("=== Drop Detector Tests ===\n");
	test_no_drops();
	test_single_drop();
	test_multi_drop();
	test_wrap_around();
	test_wrap_with_gap();
	test_duplicate_ignored();
	test_late_frame_ignored();
	test_reset_to_zero();
	test_idle_stream_restart();
	test_multiple_streams();
	printf("All drop detector tests passed.\n");
	return 0;
}
