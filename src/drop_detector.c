/* SPDX-License-Identifier: Apache-2.0 */
#include "drop_detector.h"
#include <string.h>

void drop_tracker_init(struct sv_drop_tracker *dt)
{
	memset(dt, 0, sizeof(*dt));
}

static struct sv_drop_state *find_or_create(struct sv_drop_tracker *dt,
					    const struct sv_frame_info *info)
{
	for (int i = 0; i < dt->num_streams; i++) {
		struct sv_drop_state *s = &dt->streams[i];
		if (s->id.app_id == info->app_id &&
		    strcmp(s->id.sv_id, info->sv_id) == 0)
			return s;
	}

	if (dt->num_streams >= SV_MAX_STREAMS)
		return NULL;

	struct sv_drop_state *s = &dt->streams[dt->num_streams++];
	s->id.app_id = info->app_id;
	memcpy(s->id.sv_id, info->sv_id, SV_SVID_MAX_LEN);
	s->id.sv_id[SV_SVID_MAX_LEN - 1] = '\0';
	s->initialized = 0;
	atomic_store(&s->frames_received, 0);
	atomic_store(&s->frames_dropped, 0);
	return s;
}

int drop_tracker_process(struct sv_drop_tracker *dt,
			 const struct sv_frame_info *info)
{
	struct sv_drop_state *s = find_or_create(dt, info);
	if (!s)
		return -1;

	atomic_fetch_add_explicit(&s->frames_received, 1,
				 memory_order_relaxed);

	if (!s->initialized) {
		s->last_smp_cnt = info->smp_cnt;
		s->initialized = 1;
		return 0;
	}

	uint16_t expected = (s->last_smp_cnt + 1) & 0xFFFF;
	s->last_smp_cnt = info->smp_cnt;

	if (info->smp_cnt == expected)
		return 0;

	uint16_t gap = (info->smp_cnt - expected) & 0xFFFF;
	atomic_fetch_add_explicit(&s->frames_dropped, gap,
				 memory_order_relaxed);
	return (int)gap;
}

const struct sv_drop_state *drop_tracker_find(const struct sv_drop_tracker *dt,
					      uint16_t app_id,
					      const char *sv_id)
{
	for (int i = 0; i < dt->num_streams; i++) {
		const struct sv_drop_state *s = &dt->streams[i];
		if (s->id.app_id == app_id &&
		    strcmp(s->id.sv_id, sv_id) == 0)
			return s;
	}
	return NULL;
}
