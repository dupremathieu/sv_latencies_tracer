/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}

static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)(v & 0xFF);
		v >>= 8;
	}
}

static uint16_t get_u16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

ssize_t proto_serialize_agent_batch(const struct proto_agent_record *records,
				    int count, uint8_t **out_buf)
{
	size_t payload_len = 1 + (size_t)count * PROTO_AGENT_RECORD_SIZE;
	size_t total = 4 + payload_len;
	uint8_t *buf = malloc(total);
	if (!buf)
		return -1;

	put_u32(buf, (uint32_t)payload_len);
	buf[4] = PROTO_SRC_AGENT;

	uint8_t *p = buf + 5;
	for (int i = 0; i < count; i++) {
		const struct proto_agent_record *r = &records[i];
		put_u16(p, r->app_id);
		p += 2;
		memset(p, 0, SV_SVID_MAX_LEN);
		memcpy(p, r->sv_id, SV_SVID_MAX_LEN);
		p += SV_SVID_MAX_LEN;
		put_u16(p, r->smp_cnt);
		p += 2;
		put_u64(p, r->hw_ts.sec);
		p += 8;
		put_u32(p, r->hw_ts.nsec);
		p += 4;
	}

	*out_buf = buf;
	return (ssize_t)total;
}

ssize_t proto_serialize_subscriber_batch(
	const struct proto_subscriber_record *records,
	int count, uint8_t **out_buf)
{
	size_t payload_len = 1 + (size_t)count * PROTO_SUBSCRIBER_RECORD_SIZE;
	size_t total = 4 + payload_len;
	uint8_t *buf = malloc(total);
	if (!buf)
		return -1;

	put_u32(buf, (uint32_t)payload_len);
	buf[4] = PROTO_SRC_SUBSCRIBER;

	uint8_t *p = buf + 5;
	for (int i = 0; i < count; i++) {
		const struct proto_subscriber_record *r = &records[i];
		put_u16(p, r->app_id);
		p += 2;
		memset(p, 0, SV_SVID_MAX_LEN);
		memcpy(p, r->sv_id, SV_SVID_MAX_LEN);
		p += SV_SVID_MAX_LEN;
		put_u16(p, r->smp_cnt);
		p += 2;
		put_u64(p, r->app_ts.sec);
		p += 8;
		put_u32(p, r->app_ts.nsec);
		p += 4;
		put_u64(p, r->parsed_ts.sec);
		p += 8;
		put_u32(p, r->parsed_ts.nsec);
		p += 4;
	}

	*out_buf = buf;
	return (ssize_t)total;
}

int proto_deserialize_batch(const uint8_t *buf, size_t buf_len,
			    uint8_t *src_type,
			    struct proto_agent_record *agent_out,
			    struct proto_subscriber_record *sub_out,
			    int max_records)
{
	if (buf_len < 1)
		return -1;

	*src_type = buf[0];
	const uint8_t *p = buf + 1;
	size_t remaining = buf_len - 1;

	if (*src_type == PROTO_SRC_AGENT) {
		int count = (int)(remaining / PROTO_AGENT_RECORD_SIZE);
		if (count > max_records)
			count = max_records;

		for (int i = 0; i < count; i++) {
			struct proto_agent_record *r = &agent_out[i];
			r->app_id = get_u16(p);
			p += 2;
			memcpy(r->sv_id, p, SV_SVID_MAX_LEN);
			r->sv_id[SV_SVID_MAX_LEN - 1] = '\0';
			p += SV_SVID_MAX_LEN;
			r->smp_cnt = get_u16(p);
			p += 2;
			r->hw_ts.sec = get_u64(p);
			p += 8;
			r->hw_ts.nsec = get_u32(p);
			p += 4;
		}
		return count;
	} else if (*src_type == PROTO_SRC_SUBSCRIBER) {
		int count = (int)(remaining / PROTO_SUBSCRIBER_RECORD_SIZE);
		if (count > max_records)
			count = max_records;

		for (int i = 0; i < count; i++) {
			struct proto_subscriber_record *r = &sub_out[i];
			r->app_id = get_u16(p);
			p += 2;
			memcpy(r->sv_id, p, SV_SVID_MAX_LEN);
			r->sv_id[SV_SVID_MAX_LEN - 1] = '\0';
			p += SV_SVID_MAX_LEN;
			r->smp_cnt = get_u16(p);
			p += 2;
			r->app_ts.sec = get_u64(p);
			p += 8;
			r->app_ts.nsec = get_u32(p);
			p += 4;
			r->parsed_ts.sec = get_u64(p);
			p += 8;
			r->parsed_ts.nsec = get_u32(p);
			p += 4;
		}
		return count;
	}

	return -1;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = recv(fd, p, len, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int proto_send_batch(int fd, const uint8_t *buf, size_t len)
{
	return send_all(fd, buf, len);
}

ssize_t proto_recv_batch(int fd, uint8_t **out_buf)
{
	uint8_t len_buf[4];
	if (recv_all(fd, len_buf, 4) < 0)
		return -1;

	uint32_t payload_len = get_u32(len_buf);
	if (payload_len > 1024 * 1024) /* sanity limit: 1MB */
		return -1;

	uint8_t *buf = malloc(payload_len);
	if (!buf)
		return -1;

	if (recv_all(fd, buf, payload_len) < 0) {
		free(buf);
		return -1;
	}

	*out_buf = buf;
	return (ssize_t)payload_len;
}
