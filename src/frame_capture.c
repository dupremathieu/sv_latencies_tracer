/* SPDX-License-Identifier: Apache-2.0 */
#include "frame_capture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * BPF filter: accept EtherType 0x88BA (SV) with or without 802.1Q VLAN tag.
 *
 * Equivalent tcpdump filter: "ether proto 0x88ba"
 */
static struct sock_filter sv_bpf_filter[] = {
	/* Load half-word at offset 12 (EtherType) */
	BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
	/* If 0x88BA, accept */
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SV_ETHERTYPE, 3, 0),
	/* If 0x8100 (VLAN), check inner EtherType */
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VLAN_ETHERTYPE, 0, 3),
	BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SV_ETHERTYPE, 0, 1),
	/* Accept */
	BPF_STMT(BPF_RET | BPF_K, (uint32_t)-1),
	/* Reject */
	BPF_STMT(BPF_RET | BPF_K, 0),
};

static clockid_t phc_path_to_clockid(const char *phc_path)
{
	int fd = open(phc_path, O_RDONLY);
	if (fd < 0)
		return CLOCK_REALTIME;

	/* FD-based clockid: ~fd << 3 | 3 */
	clockid_t clkid = (~(clockid_t)fd << 3) | 3;

	/* We intentionally do NOT close fd — it must remain open for
	 * clock_gettime() to work with this clockid. */
	return clkid;
}

int capture_discover_phc(const char *ifname, char *buf, size_t buflen)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	struct ethtool_ts_info tsi = { .cmd = ETHTOOL_GET_TS_INFO };
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_data = (void *)&tsi;

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		close(fd);
		return -1;
	}
	close(fd);

	if (tsi.phc_index < 0)
		return -1;

	snprintf(buf, buflen, "/dev/ptp%d", tsi.phc_index);
	return 0;
}

int capture_open(struct sv_capture_ctx *ctx, const char *ifname,
		 const char *phc_path)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->sock_fd = -1;

	ctx->if_index = (int)if_nametoindex(ifname);
	if (ctx->if_index == 0) {
		fprintf(stderr, "capture: interface '%s' not found\n", ifname);
		return -1;
	}

	ctx->sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (ctx->sock_fd < 0) {
		perror("capture: socket");
		return -1;
	}

	/* Bind to the specific interface */
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = ctx->if_index,
	};
	if (bind(ctx->sock_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		perror("capture: bind");
		goto err;
	}

	/* Attach BPF filter */
	struct sock_fprog bpf = {
		.len = sizeof(sv_bpf_filter) / sizeof(sv_bpf_filter[0]),
		.filter = sv_bpf_filter,
	};
	if (setsockopt(ctx->sock_fd, SOL_SOCKET, SO_ATTACH_FILTER,
		       &bpf, sizeof(bpf)) < 0) {
		perror("capture: SO_ATTACH_FILTER");
		goto err;
	}

	/* Enable hardware timestamping */
	int ts_flags = SOF_TIMESTAMPING_RAW_HARDWARE |
		       SOF_TIMESTAMPING_RX_HARDWARE |
		       SOF_TIMESTAMPING_SOFTWARE;
	if (setsockopt(ctx->sock_fd, SOL_SOCKET, SO_TIMESTAMPING,
		       &ts_flags, sizeof(ts_flags)) < 0) {
		perror("capture: SO_TIMESTAMPING");
		/* Non-fatal — SW timestamps will still work */
	}

	/* Also enable HW timestamping on the NIC via ioctl */
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	struct hwtstamp_config hwcfg = {
		.tx_type = HWTSTAMP_TX_OFF,
		.rx_filter = HWTSTAMP_FILTER_ALL,
	};
	ifr.ifr_data = (void *)&hwcfg;
	if (ioctl(ctx->sock_fd, SIOCSHWTSTAMP, &ifr) < 0) {
		/* Non-fatal — HW timestamps might already be configured or
		 * unsupported */
	}

	/* Set up PHC clock */
	if (phc_path) {
		ctx->phc_clockid = phc_path_to_clockid(phc_path);
	} else {
		/* Try auto-detection */
		char auto_path[64];
		if (capture_discover_phc(ifname, auto_path,
					 sizeof(auto_path)) == 0) {
			ctx->phc_clockid = phc_path_to_clockid(auto_path);
		} else {
			ctx->phc_clockid = CLOCK_REALTIME;
		}
	}

	return 0;

err:
	close(ctx->sock_fd);
	ctx->sock_fd = -1;
	return -1;
}

int capture_recv(struct sv_capture_ctx *ctx, struct sv_captured_frame *frame)
{
	struct iovec iov = {
		.iov_base = frame->data,
		.iov_len = sizeof(frame->data),
	};

	/* Ancillary data buffer for timestamps */
	char cmsg_buf[512];

	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = cmsg_buf,
		.msg_controllen = sizeof(cmsg_buf),
	};

	ssize_t n = recvmsg(ctx->sock_fd, &msg, 0);
	if (n < 0)
		return -1;

	/* Record application timestamp immediately */
	struct timespec app_now;
	clock_gettime(ctx->phc_clockid, &app_now);
	frame->app_ts = timespec_to_svts(&app_now);

	frame->len = (size_t)n;

	/* Extract hardware timestamp from cmsg */
	frame->hw_ts = (struct sv_timestamp){0, 0};
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
	     cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type == SO_TIMESTAMPING) {
			struct timespec *stamps = (struct timespec *)CMSG_DATA(cm);
			/*
			 * stamps[0] = software timestamp
			 * stamps[1] = deprecated
			 * stamps[2] = hardware timestamp
			 */
			if (stamps[2].tv_sec != 0 || stamps[2].tv_nsec != 0) {
				frame->hw_ts = timespec_to_svts(&stamps[2]);
			} else if (stamps[0].tv_sec != 0 ||
				   stamps[0].tv_nsec != 0) {
				/* Fallback to software timestamp */
				frame->hw_ts = timespec_to_svts(&stamps[0]);
			}
			break;
		}
	}

	/* If no timestamp from cmsg, use app timestamp as fallback */
	if (frame->hw_ts.sec == 0 && frame->hw_ts.nsec == 0)
		frame->hw_ts = frame->app_ts;

	return 0;
}

void capture_close(struct sv_capture_ctx *ctx)
{
	if (ctx->sock_fd >= 0) {
		close(ctx->sock_fd);
		ctx->sock_fd = -1;
	}
}
