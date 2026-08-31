/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_FRAME_CAPTURE_H
#define SV_FRAME_CAPTURE_H

#include <stdbool.h>
#include "common.h"

#define SV_FRAME_MAX_LEN 1522

struct sv_capture_ctx {
	int sock_fd;
	int phc_fd;
	int if_index;
	clockid_t phc_clockid;
	bool hw_timestamps_requested;
	bool restore_hwtstamp_config;
	int saved_hwtstamp_flags;
	int saved_hwtstamp_tx_type;
	int saved_hwtstamp_rx_filter;
	int configured_hwtstamp_tx_type;
	int configured_hwtstamp_rx_filter;
	char ifname[16];
	bool promisc_enabled;  /* true while this socket owns a promisc membership */
};

struct sv_captured_frame {
	uint8_t            data[SV_FRAME_MAX_LEN];
	size_t             len;
	struct sv_timestamp hw_rx_ts;  /* Raw hardware timestamp (PHC domain) */
	struct sv_timestamp sw_rx_ts;  /* Software RX timestamp (CLOCK_REALTIME) */
	struct sv_timestamp app_phc_ts;
	struct sv_timestamp app_realtime_ts;
	bool                have_hw_rx_ts;
	bool                have_sw_rx_ts;
	bool                have_app_phc_ts;
	struct sv_timestamp rx_ts;     /* Selected hardware/software RX timestamp */
	struct sv_timestamp app_ts;    /* Application timestamp (right after recvmsg) */
	clockid_t          timestamp_clockid; /* Clock used for app_ts and parsed_ts */
	enum sv_timestamp_source timestamp_source;
};

/*
 * Open a raw AF_PACKET socket with RX timestamp requests and an SV BPF filter.
 * Device-global hardware timestamp configuration is left unchanged.
 *
 * @param ctx       Output capture context.
 * @param ifname    Network interface name.
 * @param phc_path  PHC device path (e.g. "/dev/ptp0"), or NULL for auto-discovery.
 * @param enable_hw_timestamps  Configure the device RX filter to timestamp all frames.
 * @return 0 on success, -1 on error.
 */
int capture_open(struct sv_capture_ctx *ctx, const char *ifname,
		     const char *phc_path, int vlan_id,
		     bool enable_hw_timestamps);

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
