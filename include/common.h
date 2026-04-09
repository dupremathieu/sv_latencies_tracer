/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_COMMON_H
#define SV_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define SV_ETHERTYPE      0x88BA
#define VLAN_ETHERTYPE    0x8100
#define SV_SVID_MAX_LEN   65
#define SV_HISTOGRAM_BINS 501   /* 0..500 µs inclusive */
#define SV_MAX_STREAMS    64

struct sv_stream_id {
	uint16_t app_id;
	char     sv_id[SV_SVID_MAX_LEN];
};

struct sv_frame_info {
	uint16_t app_id;
	char     sv_id[SV_SVID_MAX_LEN];
	uint16_t smp_cnt;
};

struct sv_timestamp {
	uint64_t sec;
	uint32_t nsec;
};

static inline int64_t ts_delta_us(const struct sv_timestamp *a,
				  const struct sv_timestamp *b)
{
	int64_t ds = (int64_t)a->sec - (int64_t)b->sec;
	int64_t dn = (int64_t)a->nsec - (int64_t)b->nsec;
	return ds * 1000000 + dn / 1000;
}

static inline void sv_copy_svid(char *dst, const char *src)
{
	memcpy(dst, src, SV_SVID_MAX_LEN);
	dst[SV_SVID_MAX_LEN - 1] = '\0';
}

static inline struct sv_timestamp timespec_to_svts(const struct timespec *ts)
{
	return (struct sv_timestamp){
		.sec = (uint64_t)ts->tv_sec,
		.nsec = (uint32_t)ts->tv_nsec,
	};
}

#endif /* SV_COMMON_H */
