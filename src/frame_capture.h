/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_FRAME_CAPTURE_H
#define SV_FRAME_CAPTURE_H

#include "common.h"

#define SV_FRAME_MAX_LEN 1518

struct sv_capture_ctx {
	int sock_fd;
	int if_index;
	clockid_t phc_clockid;
};

struct sv_captured_frame {
	uint8_t            data[SV_FRAME_MAX_LEN];
	size_t             len;
	struct sv_timestamp hw_ts;     /* NIC hardware timestamp */
	struct sv_timestamp app_ts;    /* Application timestamp (right after recvmsg) */
};

/*
 * Open a raw AF_PACKET socket on the given interface with HW timestamping
 * and BPF filter for EtherType 0x88BA.
 *
 * @param ctx       Output capture context.
 * @param ifname    Network interface name.
 * @param phc_path  PHC device path (e.g. "/dev/ptp0"), or NULL for CLOCK_REALTIME.
 * @return 0 on success, -1 on error.
 */
int capture_open(struct sv_capture_ctx *ctx, const char *ifname,
		 const char *phc_path);

/*
 * Receive one SV frame with timestamps.
 * Blocks until a frame is available.
 * @return 0 on success, -1 on error.
 */
int capture_recv(struct sv_capture_ctx *ctx, struct sv_captured_frame *frame);

void capture_close(struct sv_capture_ctx *ctx);

/*
 * Discover the PHC device path for a given network interface.
 * @param ifname    Network interface name.
 * @param buf       Output buffer for the path (e.g. "/dev/ptp0").
 * @param buflen    Size of buf.
 * @return 0 on success, -1 if not found.
 */
int capture_discover_phc(const char *ifname, char *buf, size_t buflen);

#endif /* SV_FRAME_CAPTURE_H */
