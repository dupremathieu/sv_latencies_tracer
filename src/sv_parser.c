/* SPDX-License-Identifier: Apache-2.0 */
#include "sv_parser.h"
#include <string.h>
#include <arpa/inet.h>

/*
 * IEC 61850 SV frame layout (after Ethernet header):
 *
 *   Offset  Field
 *   0-1     appID       (uint16, big-endian)
 *   2-3     length      (uint16, big-endian, total PDU length including these 4 bytes)
 *   4-5     reserved1
 *   6-7     reserved2
 *   8+      savPdu      (ASN.1 BER-encoded)
 *
 * savPdu structure (tags per IEC 61850-9-2):
 *   Tag 0x60  savPdu SEQUENCE
 *     Tag 0x80  noASDU (INTEGER, number of ASDUs)
 *     Tag 0xA2  seqASDU SEQUENCE OF
 *       Tag 0x30  ASDU SEQUENCE
 *         Tag 0x80  svID (VisibleString)
 *         Tag 0x82  smpCnt (INT16U, 2 bytes)
 *         ...other fields we skip...
 */

/* BER tag constants for SV PDU */
#define TAG_SAVPDU       0x60
#define TAG_NO_ASDU      0x80
#define TAG_SEQ_ASDU     0xA2
#define TAG_ASDU         0x30
#define TAG_SVID         0x80
#define TAG_DATASET       0x81
#define TAG_SMPCNT       0x82

static int ber_read_length(const uint8_t *buf, size_t remaining,
			   size_t *length, size_t *hdr_len)
{
	if (remaining < 1)
		return -1;

	if (buf[0] < 0x80) {
		*length = buf[0];
		*hdr_len = 1;
		return 0;
	}

	size_t n_octets = buf[0] & 0x7F;
	if (n_octets == 0 || n_octets > 4 || remaining < 1 + n_octets)
		return -1;

	size_t len = 0;
	for (size_t i = 0; i < n_octets; i++)
		len = (len << 8) | buf[1 + i];

	*length = len;
	*hdr_len = 1 + n_octets;
	return 0;
}

/*
 * Read a BER TLV header: tag byte + length.
 * Returns 0 on success, -1 on error.
 * On success, *tag is the tag byte, *content_len is the value length,
 * and *hdr_size is the total header size (1 + length encoding).
 */
static int ber_read_tlv(const uint8_t *buf, size_t remaining,
			uint8_t *tag, size_t *content_len, size_t *hdr_size)
{
	if (remaining < 2)
		return -1;

	*tag = buf[0];
	size_t len_hdr;
	if (ber_read_length(buf + 1, remaining - 1, content_len, &len_hdr) < 0)
		return -1;

	*hdr_size = 1 + len_hdr;
	if (*hdr_size + *content_len > remaining)
		return -1;

	return 0;
}

static int parse_asdu(const uint8_t *buf, size_t len,
		      struct sv_frame_info *info)
{
	size_t pos = 0;
	int found_svid = 0, found_smpcnt = 0;

	while (pos < len && !(found_svid && found_smpcnt)) {
		uint8_t tag;
		size_t content_len, hdr_size;

		if (ber_read_tlv(buf + pos, len - pos,
				 &tag, &content_len, &hdr_size) < 0)
			return -1;

		const uint8_t *content = buf + pos + hdr_size;

		switch (tag) {
		case TAG_SVID:
			if (content_len >= SV_SVID_MAX_LEN)
				return -1;
			memcpy(info->sv_id, content, content_len);
			info->sv_id[content_len] = '\0';
			found_svid = 1;
			break;
		case TAG_SMPCNT:
			if (content_len != 2)
				return -1;
			info->smp_cnt = ((uint16_t)content[0] << 8) | content[1];
			found_smpcnt = 1;
			break;
		default:
			break;
		}

		pos += hdr_size + content_len;
	}

	return (found_svid && found_smpcnt) ? 0 : -1;
}

int sv_parse(const uint8_t *frame, size_t frame_len,
	     struct sv_frame_info *info)
{
	if (!frame || !info || frame_len < 14)
		return -1;

	memset(info, 0, sizeof(*info));

	/* Determine payload offset based on VLAN tagging */
	size_t eth_hdr_len;
	uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];

	if (ethertype == VLAN_ETHERTYPE) {
		if (frame_len < 18)
			return -1;
		ethertype = ((uint16_t)frame[16] << 8) | frame[17];
		eth_hdr_len = 18;
	} else {
		eth_hdr_len = 14;
	}

	if (ethertype != SV_ETHERTYPE)
		return -1;

	const uint8_t *sv = frame + eth_hdr_len;
	size_t sv_len = frame_len - eth_hdr_len;

	/* SV header: appID(2) + length(2) + reserved1(2) + reserved2(2) = 8 bytes */
	if (sv_len < 8)
		return -1;

	info->app_id = ((uint16_t)sv[0] << 8) | sv[1];

	/* Parse savPdu starting after the 8-byte SV header */
	const uint8_t *pdu = sv + 8;
	size_t pdu_len = sv_len - 8;

	/* Expect savPdu SEQUENCE (tag 0x60) */
	uint8_t tag;
	size_t content_len, hdr_size;

	if (ber_read_tlv(pdu, pdu_len, &tag, &content_len, &hdr_size) < 0)
		return -1;
	if (tag != TAG_SAVPDU)
		return -1;

	const uint8_t *savpdu_content = pdu + hdr_size;
	size_t savpdu_remaining = content_len;
	size_t pos = 0;

	/* Skip noASDU (tag 0x80) */
	if (ber_read_tlv(savpdu_content + pos, savpdu_remaining - pos,
			 &tag, &content_len, &hdr_size) < 0)
		return -1;
	if (tag != TAG_NO_ASDU)
		return -1;
	pos += hdr_size + content_len;

	/* Expect seqASDU (tag 0xA2) */
	if (ber_read_tlv(savpdu_content + pos, savpdu_remaining - pos,
			 &tag, &content_len, &hdr_size) < 0)
		return -1;
	if (tag != TAG_SEQ_ASDU)
		return -1;

	const uint8_t *seq_content = savpdu_content + pos + hdr_size;
	size_t seq_len = content_len;

	/* Parse first ASDU (tag 0x30) */
	if (ber_read_tlv(seq_content, seq_len, &tag, &content_len, &hdr_size) < 0)
		return -1;
	if (tag != TAG_ASDU)
		return -1;

	return parse_asdu(seq_content + hdr_size, content_len, info);
}
