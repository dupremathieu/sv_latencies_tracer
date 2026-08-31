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
static int phc_path_to_clockid(const char *phc_path, clockid_t *clock_id)
{
	int fd = open(phc_path, O_RDONLY);
	if (fd < 0)
		return -1;

	/* FD-based clockid: ~fd << 3 | 3 */
	*clock_id = (~(clockid_t)fd << 3) | 3;

	/* We intentionally do NOT close fd — it must remain open for
	 * clock_gettime() to work with this clockid. */
	return fd;
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

static void restore_device_hwtstamp(struct sv_capture_ctx *ctx)
{
	if (!ctx->restore_hwtstamp_config || ctx->sock_fd < 0)
		return;

	struct hwtstamp_config current = { 0 };
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	memcpy(ifr.ifr_name, ctx->ifname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (void *)&current;
	if (ioctl(ctx->sock_fd, SIOCGHWTSTAMP, &ifr) < 0) {
		perror("capture: verify SIOCGHWTSTAMP before restoring");
		ctx->restore_hwtstamp_config = false;
		return;
	}
	if (current.tx_type != ctx->configured_hwtstamp_tx_type ||
	    current.rx_filter != ctx->configured_hwtstamp_rx_filter) {
		fprintf(stderr,
			"capture: '%s' device timestamp config changed externally; "
			"not restoring it\n", ctx->ifname);
		ctx->restore_hwtstamp_config = false;
		return;
	}

	struct hwtstamp_config hwcfg = {
		.flags = ctx->saved_hwtstamp_flags,
		.tx_type = ctx->saved_hwtstamp_tx_type,
		.rx_filter = ctx->saved_hwtstamp_rx_filter,
	};
	ifr.ifr_data = (void *)&hwcfg;
	if (ioctl(ctx->sock_fd, SIOCSHWTSTAMP, &ifr) < 0) {
		perror("capture: restore SIOCSHWTSTAMP");
	} else {
		fprintf(stderr,
			"capture: restored '%s' device timestamp config "
			"(tx_type=%d, rx_filter=%d)\n",
			ctx->ifname, hwcfg.tx_type, hwcfg.rx_filter);
	}
	ctx->restore_hwtstamp_config = false;
}

int capture_open(struct sv_capture_ctx *ctx, const char *ifname,
		 const char *phc_path, int vlan_id,
		 bool enable_hw_timestamps)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->sock_fd = -1;
	ctx->phc_fd = -1;
	ctx->phc_clockid = CLOCK_REALTIME;
	snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", ifname);

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

	/*
	 * SV frames are commonly sent to a multicast MAC address.  tcpdump
	 * enables promiscuous mode by default, but an AF_PACKET socket does not.
	 * Use a socket-scoped membership so multicast SV frames reach us without
	 * changing the interface flags for other users.
	 */
	struct packet_mreq promisc = {
		.mr_ifindex = ctx->if_index,
		.mr_type = PACKET_MR_PROMISC,
	};
	if (setsockopt(ctx->sock_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
		       &promisc, sizeof(promisc)) < 0) {
		perror("capture: PACKET_ADD_MEMBERSHIP(PACKET_MR_PROMISC)");
		goto err;
	}
	ctx->promisc_enabled = true;

	/* Attach a filter for SV, optionally restricted to one VLAN. */
	struct sock_filter all_vlans[] = {
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SV_ETHERTYPE, 3, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VLAN_ETHERTYPE, 0, 3),
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SV_ETHERTYPE, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, (uint32_t)-1),
		BPF_STMT(BPF_RET | BPF_K, 0),
	};
	struct sock_filter one_vlan[] = {
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VLAN_ETHERTYPE, 0, 6),
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 14),
		BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0FFF),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)vlan_id, 0, 3),
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SV_ETHERTYPE, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, (uint32_t)-1),
		BPF_STMT(BPF_RET | BPF_K, 0),
	};
	struct sock_filter *filter = vlan_id < 0 ? all_vlans : one_vlan;
	struct sock_fprog bpf = {
		.len = vlan_id < 0 ? sizeof(all_vlans) / sizeof(all_vlans[0])
				   : sizeof(one_vlan) / sizeof(one_vlan[0]),
		.filter = filter,
	};
	if (setsockopt(ctx->sock_fd, SOL_SOCKET, SO_ATTACH_FILTER,
		       &bpf, sizeof(bpf)) < 0) {
		perror("capture: SO_ATTACH_FILTER");
		goto err;
	}

	/* SIOCSHWTSTAMP is device-global, so preserve its external owner. */
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	struct hwtstamp_config hwcfg = { 0 };
	ifr.ifr_data = (void *)&hwcfg;
	if (ioctl(ctx->sock_fd, SIOCGHWTSTAMP, &ifr) == 0) {
		fprintf(stderr,
			"capture: preserving '%s' device timestamp config "
			"(tx_type=%d, rx_filter=%d)\n",
			ifname, hwcfg.tx_type, hwcfg.rx_filter);
		if (enable_hw_timestamps &&
		    hwcfg.rx_filter != HWTSTAMP_FILTER_ALL) {
			ctx->saved_hwtstamp_flags = hwcfg.flags;
			ctx->saved_hwtstamp_tx_type = hwcfg.tx_type;
			ctx->saved_hwtstamp_rx_filter = hwcfg.rx_filter;
			hwcfg.flags = 0;
			hwcfg.rx_filter = HWTSTAMP_FILTER_ALL;
			if (ioctl(ctx->sock_fd, SIOCSHWTSTAMP, &ifr) < 0) {
				perror("capture: enable SIOCSHWTSTAMP");
				goto err;
			}
			ctx->restore_hwtstamp_config = true;
			ctx->configured_hwtstamp_tx_type = hwcfg.tx_type;
			ctx->configured_hwtstamp_rx_filter = hwcfg.rx_filter;
			fprintf(stderr,
				"capture: enabled all-frame hardware RX timestamps "
				"on '%s' (tx_type=%d, rx_filter=%d)\n",
				ifname, hwcfg.tx_type, hwcfg.rx_filter);
		}
	} else {
		if (enable_hw_timestamps) {
			perror("capture: read SIOCGHWTSTAMP before enabling");
			goto err;
		}
		fprintf(stderr,
			"capture: device timestamp config for '%s' is unavailable; "
			"leaving it unchanged\n", ifname);
	}

	int ts_flags = SOF_TIMESTAMPING_RAW_HARDWARE |
		       SOF_TIMESTAMPING_RX_HARDWARE |
		       SOF_TIMESTAMPING_SOFTWARE |
		       SOF_TIMESTAMPING_RX_SOFTWARE;
	if (setsockopt(ctx->sock_fd, SOL_SOCKET, SO_TIMESTAMPING,
		       &ts_flags, sizeof(ts_flags)) < 0) {
		fprintf(stderr,
			"capture: hardware timestamp requests unavailable on '%s'; "
			"requesting software timestamps only\n", ifname);
		ts_flags = SOF_TIMESTAMPING_SOFTWARE |
			   SOF_TIMESTAMPING_RX_SOFTWARE;
		if (setsockopt(ctx->sock_fd, SOL_SOCKET, SO_TIMESTAMPING,
			       &ts_flags, sizeof(ts_flags)) < 0)
			perror("capture: SO_TIMESTAMPING");
	} else {
		ctx->hw_timestamps_requested = true;
	}

	/* Set up PHC clock */
	if (phc_path) {
		ctx->phc_fd = phc_path_to_clockid(phc_path, &ctx->phc_clockid);
		if (ctx->phc_fd < 0) {
			fprintf(stderr, "capture: cannot open PHC '%s', using CLOCK_REALTIME\n",
				phc_path);
			ctx->phc_clockid = CLOCK_REALTIME;
		}
	} else if (ctx->hw_timestamps_requested) {
		/* A PHC is needed only if the socket receives hardware timestamps. */
		char auto_path[64];
		if (capture_discover_phc(ifname, auto_path,
					 sizeof(auto_path)) == 0) {
			ctx->phc_fd = phc_path_to_clockid(auto_path,
							  &ctx->phc_clockid);
			if (ctx->phc_fd < 0)
				ctx->phc_clockid = CLOCK_REALTIME;
		} else {
			fprintf(stderr,
				"capture: WARNING: could not discover PHC for "
				"'%s', using CLOCK_REALTIME for app timestamps\n",
				ifname);
			ctx->phc_clockid = CLOCK_REALTIME;
		}
	} else {
		ctx->phc_clockid = CLOCK_REALTIME;
	}

	return 0;

err:
	restore_device_hwtstamp(ctx);
	close(ctx->sock_fd);
	ctx->sock_fd = -1;
	if (ctx->phc_fd >= 0) {
		close(ctx->phc_fd);
		ctx->phc_fd = -1;
	}
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

	frame->len = (size_t)n;
	frame->hw_rx_ts = (struct sv_timestamp){0, 0};
	frame->sw_rx_ts = (struct sv_timestamp){0, 0};
	frame->app_phc_ts = (struct sv_timestamp){0, 0};
	frame->app_realtime_ts = (struct sv_timestamp){0, 0};
	frame->have_hw_rx_ts = false;
	frame->have_sw_rx_ts = false;
	frame->have_app_phc_ts = false;

	/* Extract timestamps from cmsg. */
	struct sv_timestamp hw_ts = {0, 0};
	struct sv_timestamp sw_ts = {0, 0};
	int have_hw_ts = 0;
	int have_sw_ts = 0;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm;
	     cm = CMSG_NXTHDR(&msg, cm)) {
		if (cm->cmsg_level == SOL_SOCKET &&
		    cm->cmsg_type == SO_TIMESTAMPING) {
			if (cm->cmsg_len < CMSG_LEN(3 * sizeof(struct timespec)))
				continue;
			struct timespec *stamps = (struct timespec *)CMSG_DATA(cm);
			/*
			 * stamps[0] = software timestamp
			 * stamps[1] = deprecated
			 * stamps[2] = hardware timestamp
			 */
			if (stamps[2].tv_sec != 0 || stamps[2].tv_nsec != 0) {
				hw_ts = timespec_to_svts(&stamps[2]);
				have_hw_ts = 1;
			}
			if (stamps[0].tv_sec != 0 || stamps[0].tv_nsec != 0) {
				sw_ts = timespec_to_svts(&stamps[0]);
				have_sw_ts = 1;
			}
			break;
		}
	}

	/*
	 * Read both application clocks immediately after recvmsg. This preserves
	 * each RX timestamp's clock domain while allowing their elapsed durations
	 * to be compared without subtracting PHC and CLOCK_REALTIME directly.
	 */
	if (have_hw_ts && ctx->phc_fd >= 0) {
		struct timespec app_phc_now;
		if (clock_gettime(ctx->phc_clockid, &app_phc_now) == 0) {
			frame->app_phc_ts = timespec_to_svts(&app_phc_now);
			frame->have_app_phc_ts = true;
		}
	}

	frame->hw_rx_ts = hw_ts;
	frame->sw_rx_ts = sw_ts;
	frame->have_hw_rx_ts = have_hw_ts;
	frame->have_sw_rx_ts = have_sw_ts;

	/* A hardware timestamp is comparable only when its PHC is available. */
	int use_hw_ts = have_hw_ts && frame->have_app_phc_ts;
	int use_sw_ts = !use_hw_ts && have_sw_ts;
	if (have_sw_ts || !use_hw_ts) {
		struct timespec app_realtime_now;
		if (clock_gettime(CLOCK_REALTIME, &app_realtime_now) < 0)
			return -1;
		frame->app_realtime_ts = timespec_to_svts(&app_realtime_now);
	}
	if (use_hw_ts) {
		frame->rx_ts = hw_ts;
		frame->app_ts = frame->app_phc_ts;
		frame->timestamp_clockid = ctx->phc_clockid;
		frame->timestamp_source = SV_TIMESTAMP_SOURCE_HARDWARE;
	} else if (use_sw_ts) {
		frame->rx_ts = sw_ts;
		frame->app_ts = frame->app_realtime_ts;
		frame->timestamp_clockid = CLOCK_REALTIME;
		frame->timestamp_source = SV_TIMESTAMP_SOURCE_SOFTWARE;
	} else {
		frame->rx_ts = frame->app_realtime_ts;
		frame->app_ts = frame->app_realtime_ts;
		frame->timestamp_clockid = CLOCK_REALTIME;
		frame->timestamp_source = SV_TIMESTAMP_SOURCE_APPLICATION;
	}

	return 0;
}

void capture_close(struct sv_capture_ctx *ctx)
{
	if (ctx->sock_fd >= 0) {
		restore_device_hwtstamp(ctx);
		if (ctx->promisc_enabled) {
			struct packet_mreq promisc = {
				.mr_ifindex = ctx->if_index,
				.mr_type = PACKET_MR_PROMISC,
			};
			if (setsockopt(ctx->sock_fd, SOL_PACKET,
				       PACKET_DROP_MEMBERSHIP,
				       &promisc, sizeof(promisc)) < 0 && errno != ENODEV)
				perror("capture: PACKET_DROP_MEMBERSHIP(PACKET_MR_PROMISC)");
			ctx->promisc_enabled = false;
		}
		close(ctx->sock_fd);
		ctx->sock_fd = -1;
	}
	if (ctx->phc_fd >= 0) {
		close(ctx->phc_fd);
		ctx->phc_fd = -1;
	}
}
