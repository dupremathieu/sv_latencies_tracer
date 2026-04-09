/* SPDX-License-Identifier: Apache-2.0 */
#include "system_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- Link state monitor via NETLINK_ROUTE --- */

static void *link_monitor_thread(void *arg)
{
	struct sv_sysmon_ctx *ctx = arg;
	int if_index = (int)if_nametoindex(ctx->ifname);
	if (if_index == 0)
		return NULL;

	int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (fd < 0)
		return NULL;

	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK,
	};
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return NULL;
	}

	char buf[4096];
	while (ctx->running) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (struct nlmsghdr *nh = (struct nlmsghdr *)buf;
		     NLMSG_OK(nh, (size_t)n); nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type != RTM_NEWLINK)
				continue;

			struct ifinfomsg *ifi = NLMSG_DATA(nh);
			if (ifi->ifi_index != if_index)
				continue;

			int up = (ifi->ifi_flags & IFF_RUNNING) ? 1 : 0;
			atomic_store_explicit(&ctx->ms->link_up, up,
					      memory_order_relaxed);
		}
	}

	close(fd);
	return NULL;
}

/* --- Kernel oops/panic monitor via /dev/kmsg --- */

static void *kmsg_monitor_thread(void *arg)
{
	struct sv_sysmon_ctx *ctx = arg;

	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return NULL;

	/* Seek to end to only see new messages */
	lseek(fd, 0, SEEK_END);

	char line[1024];
	while (ctx->running) {
		ssize_t n = read(fd, line, sizeof(line) - 1);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				usleep(100000); /* 100ms poll */
				continue;
			}
			break;
		}
		line[n] = '\0';

		if (strstr(line, "Oops") || strstr(line, "oops"))
			atomic_fetch_add_explicit(&ctx->ms->kernel_oops_total,
						  1, memory_order_relaxed);
		if (strstr(line, "Kernel panic"))
			atomic_fetch_add_explicit(&ctx->ms->kernel_panic_total,
						  1, memory_order_relaxed);
	}

	close(fd);
	return NULL;
}

/* --- RT throttling monitor --- */

static void *rt_throttle_monitor_thread(void *arg)
{
	struct sv_sysmon_ctx *ctx = arg;

	/*
	 * Monitor RT throttling by watching
	 * /sys/kernel/debug/tracing/trace_pipe for sched_rt_runtime events,
	 * or alternatively poll /proc/sys/kernel/sched_rt_runtime_us.
	 *
	 * Here we use a simpler approach: periodically read
	 * /proc/sched_debug looking for throttled indicators.
	 * A production implementation could use ftrace.
	 */
	const char *trace_path =
		"/sys/kernel/debug/tracing/events/sched/sched_rt_runtime/enable";

	/* Try to enable the tracepoint */
	int tfd = open(trace_path, O_WRONLY);
	if (tfd >= 0) {
		(void)write(tfd, "1", 1);
		close(tfd);
	}

	const char *pipe_path = "/sys/kernel/debug/tracing/trace_pipe";
	int pfd = open(pipe_path, O_RDONLY | O_NONBLOCK);

	char buf[1024];
	while (ctx->running) {
		if (pfd >= 0) {
			ssize_t n = read(pfd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				if (strstr(buf, "sched_rt_runtime"))
					atomic_fetch_add_explicit(
						&ctx->ms->rt_throttle_total, 1,
						memory_order_relaxed);
				continue;
			}
		}
		usleep(100000);
	}

	if (pfd >= 0)
		close(pfd);
	return NULL;
}

int sysmon_start(struct sv_sysmon_ctx *ctx, struct sv_metrics_state *ms,
		 const char *ifname)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->ms = ms;
	ctx->ifname = ifname;
	ctx->running = 1;

	if (pthread_create(&ctx->link_thread, NULL, link_monitor_thread,
			   ctx) != 0) {
		perror("sysmon: link thread");
		return -1;
	}

	if (pthread_create(&ctx->kmsg_thread, NULL, kmsg_monitor_thread,
			   ctx) != 0) {
		perror("sysmon: kmsg thread");
	}

	if (pthread_create(&ctx->rt_thread, NULL, rt_throttle_monitor_thread,
			   ctx) != 0) {
		perror("sysmon: rt throttle thread");
	}

	return 0;
}

void sysmon_stop(struct sv_sysmon_ctx *ctx)
{
	ctx->running = 0;
	pthread_join(ctx->link_thread, NULL);
	pthread_join(ctx->kmsg_thread, NULL);
	pthread_join(ctx->rt_thread, NULL);
}
