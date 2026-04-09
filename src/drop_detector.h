/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_DROP_DETECTOR_H
#define SV_DROP_DETECTOR_H

#include "common.h"

struct sv_drop_state {
	struct sv_stream_id id;
	uint16_t last_smp_cnt;
	int       initialized;
	_Atomic uint64_t frames_received;
	_Atomic uint64_t frames_dropped;
};

struct sv_drop_tracker {
	struct sv_drop_state streams[SV_MAX_STREAMS];
	int num_streams;
};

void drop_tracker_init(struct sv_drop_tracker *dt);

/*
 * Process a received SV frame. Returns the number of dropped frames
 * detected (0 if none, -1 on error e.g. too many streams).
 */
int drop_tracker_process(struct sv_drop_tracker *dt,
			 const struct sv_frame_info *info);

/* Find drop state for a stream. Returns NULL if not tracked. */
const struct sv_drop_state *drop_tracker_find(const struct sv_drop_tracker *dt,
					      uint16_t app_id,
					      const char *sv_id);

#endif /* SV_DROP_DETECTOR_H */
