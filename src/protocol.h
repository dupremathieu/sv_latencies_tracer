/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_PROTOCOL_H
#define SV_PROTOCOL_H

#include <sys/types.h>
#include "common.h"

/*
 * Split-architecture batch protocol (Scenario B).
 *
 * Wire format:
 *   [4 bytes] batch length (big-endian, excluding this field)
 *   [1 byte]  source type (PROTO_SRC_AGENT or PROTO_SRC_SUBSCRIBER)
 *   [N]       array of records
 *
 * Agent record (83 bytes):
 *   uint16_t  app_id
 *   char[65]  sv_id (null-terminated, padded)
 *   uint16_t  smp_cnt
 *   uint64_t  hw_ts_sec
 *   uint32_t  hw_ts_nsec
 *
 * Subscriber record (95 bytes):
 *   uint16_t  app_id
 *   char[65]  sv_id
 *   uint16_t  smp_cnt
 *   uint64_t  app_ts_sec
 *   uint32_t  app_ts_nsec
 *   uint64_t  parsed_ts_sec
 *   uint32_t  parsed_ts_nsec
 */

#define PROTO_SRC_AGENT       0x01
#define PROTO_SRC_SUBSCRIBER  0x02

#define PROTO_AGENT_RECORD_SIZE      83
#define PROTO_SUBSCRIBER_RECORD_SIZE 95

struct proto_agent_record {
	uint16_t          app_id;
	char              sv_id[SV_SVID_MAX_LEN];
	uint16_t          smp_cnt;
	struct sv_timestamp hw_ts;
};

struct proto_subscriber_record {
	uint16_t          app_id;
	char              sv_id[SV_SVID_MAX_LEN];
	uint16_t          smp_cnt;
	struct sv_timestamp app_ts;
	struct sv_timestamp parsed_ts;
};

/*
 * Serialize a batch of agent records into a wire-format buffer.
 * Returns the total buffer size, or -1 on error. Caller frees *out_buf.
 */
ssize_t proto_serialize_agent_batch(const struct proto_agent_record *records,
				    int count, uint8_t **out_buf);

ssize_t proto_serialize_subscriber_batch(
	const struct proto_subscriber_record *records,
	int count, uint8_t **out_buf);

/*
 * Deserialize a batch from wire format.
 * @param buf       Buffer starting after the 4-byte length prefix.
 * @param buf_len   Length of the buffer.
 * @param src_type  Output: PROTO_SRC_AGENT or PROTO_SRC_SUBSCRIBER.
 * @param agent_out Output array for agent records (if src_type is AGENT).
 * @param sub_out   Output array for subscriber records (if src_type is SUB).
 * @param max_records Max records to decode.
 * @return Number of records decoded, or -1 on error.
 */
int proto_deserialize_batch(const uint8_t *buf, size_t buf_len,
			    uint8_t *src_type,
			    struct proto_agent_record *agent_out,
			    struct proto_subscriber_record *sub_out,
			    int max_records);

/*
 * Send a complete batch over a TCP socket (length-prefixed).
 * Returns 0 on success, -1 on error.
 */
int proto_send_batch(int fd, const uint8_t *buf, size_t len);

/*
 * Receive a complete batch from a TCP socket.
 * Returns the payload size (allocated in *out_buf, caller frees), or -1.
 */
ssize_t proto_recv_batch(int fd, uint8_t **out_buf);

#endif /* SV_PROTOCOL_H */
