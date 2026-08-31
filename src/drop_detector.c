/* SPDX-License-Identifier: Apache-2.0 */
#include "drop_detector.h"
#include <string.h>

void drop_tracker_init(struct sv_drop_tracker *dt)
{
	memset(dt, 0, sizeof(*dt));
	pthread_mutex_init(&dt->lock, NULL);
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
	s->have_last_rx_ts = 0;
	atomic_store(&s->frames_received, 0);
	atomic_store(&s->frames_dropped, 0);
	return s;
}

int drop_tracker_process(struct sv_drop_tracker *dt,
			 const struct sv_frame_info *info)

{
	return drop_tracker_process_at(dt, info, NULL);
}

int drop_tracker_process_at(struct sv_drop_tracker *dt,
			    const struct sv_frame_info *info,
			    const struct sv_timestamp *rx_ts)
{
	pthread_mutex_lock(&dt->lock);
	struct sv_drop_state *s = find_or_create(dt, info);
	if (!s) {
		pthread_mutex_unlock(&dt->lock);
		return -1;
	}

	atomic_fetch_add_explicit(&s->frames_received, 1,
				 memory_order_relaxed);

	if (!s->initialized) {
		s->last_smp_cnt = info->smp_cnt;
		s->initialized = 1;
		if (rx_ts) {
			s->last_rx_ts = *rx_ts;
			s->have_last_rx_ts = 1;
		}
		pthread_mutex_unlock(&dt->lock);
		return 0;
	}

	if (rx_ts && s->have_last_rx_ts &&
	    ts_delta_ns(rx_ts, &s->last_rx_ts) > SV_STREAM_IDLE_RESET_NS) {
		s->last_smp_cnt = info->smp_cnt;
		s->last_rx_ts = *rx_ts;
		pthread_mutex_unlock(&dt->lock);
		return 0;
	}
	if (rx_ts) {
		s->last_rx_ts = *rx_ts;
		s->have_last_rx_ts = 1;
	}

	/* A producer restart can reset the sequence counter to zero. */
	if (info->smp_cnt == 0 &&
	    s->last_smp_cnt != SV_SMP_CNT_MODULUS - 1) {
		s->last_smp_cnt = 0;
		pthread_mutex_unlock(&dt->lock);
		return 0;
	}

	uint16_t advance = (info->smp_cnt + SV_SMP_CNT_MODULUS -
			    s->last_smp_cnt) % SV_SMP_CNT_MODULUS;

	/* Ignore duplicate or late frames without moving the sequence forward. */
	if (advance == 0 || advance > SV_SMP_CNT_MODULUS / 2) {
		pthread_mutex_unlock(&dt->lock);
		return 0;
	}

	s->last_smp_cnt = info->smp_cnt;
	uint16_t gap = advance - 1;
	atomic_fetch_add_explicit(&s->frames_dropped, gap,
				 memory_order_relaxed);
	pthread_mutex_unlock(&dt->lock);
	return (int)gap;
}

const struct sv_drop_state *drop_tracker_find(const struct sv_drop_tracker *dt,
					      uint16_t app_id,
					      const char *sv_id)
{
	pthread_mutex_lock((pthread_mutex_t *)&dt->lock);
	for (int i = 0; i < dt->num_streams; i++) {
		const struct sv_drop_state *s = &dt->streams[i];
		if (s->id.app_id == app_id &&
		    strcmp(s->id.sv_id, sv_id) == 0) {
			pthread_mutex_unlock((pthread_mutex_t *)&dt->lock);
			return s;
		}
	}
	pthread_mutex_unlock((pthread_mutex_t *)&dt->lock);
	return NULL;
}
