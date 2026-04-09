/* SPDX-License-Identifier: Apache-2.0 */
#include "sv_parser.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Build a minimal SV Ethernet frame for testing.
 *
 * Ethernet header: dst(6) + src(6) + ethertype(2) = 14 bytes
 * SV header: appID(2) + length(2) + reserved(4) = 8 bytes
 * savPdu:
 *   0x60 LEN                       (savPdu SEQUENCE)
 *     0x80 0x01 0x01                (noASDU = 1)
 *     0xA2 LEN                     (seqASDU)
 *       0x30 LEN                   (ASDU)
 *         0x80 LEN svID...         (svID)
 *         0x82 0x02 smpCnt(2)      (smpCnt)
 */
static size_t build_sv_frame(uint8_t *buf, size_t buflen,
			     uint16_t app_id, const char *sv_id,
			     uint16_t smp_cnt, int with_vlan)
{
	size_t pos = 0;

	/* Destination MAC */
	memset(buf + pos, 0x01, 6);
	pos += 6;
	/* Source MAC */
	memset(buf + pos, 0x02, 6);
	pos += 6;

	if (with_vlan) {
		/* 802.1Q tag */
		buf[pos++] = 0x81;
		buf[pos++] = 0x00;
		buf[pos++] = 0x00; /* VLAN ID high */
		buf[pos++] = 0x64; /* VLAN ID = 100 */
	}

	/* EtherType: 0x88BA */
	buf[pos++] = 0x88;
	buf[pos++] = 0xBA;

	/* SV header */
	size_t sv_hdr_pos = pos;
	buf[pos++] = (uint8_t)(app_id >> 8);
	buf[pos++] = (uint8_t)(app_id);

	/* Length placeholder — we'll fill in later */
	size_t len_pos = pos;
	buf[pos++] = 0;
	buf[pos++] = 0;

	/* Reserved */
	buf[pos++] = 0;
	buf[pos++] = 0;
	buf[pos++] = 0;
	buf[pos++] = 0;

	/* savPdu content */
	size_t svid_len = strlen(sv_id);

	/* ASDU content: svID TLV + smpCnt TLV */
	size_t asdu_content_len = 2 + svid_len + 4; /* tag+len+svid + tag+len+2 */
	size_t asdu_len = 2 + asdu_content_len; /* tag + 1-byte len + content */
	size_t seq_content_len = asdu_len;
	size_t seq_len = 2 + seq_content_len;
	size_t noasdu_len = 3; /* tag + len(1) + value(1) */
	size_t savpdu_content_len = noasdu_len + seq_len;

	/* savPdu SEQUENCE */
	buf[pos++] = 0x60;
	buf[pos++] = (uint8_t)savpdu_content_len;

	/* noASDU */
	buf[pos++] = 0x80;
	buf[pos++] = 0x01;
	buf[pos++] = 0x01;

	/* seqASDU */
	buf[pos++] = 0xA2;
	buf[pos++] = (uint8_t)seq_content_len;

	/* ASDU */
	buf[pos++] = 0x30;
	buf[pos++] = (uint8_t)asdu_content_len;

	/* svID */
	buf[pos++] = 0x80;
	buf[pos++] = (uint8_t)svid_len;
	memcpy(buf + pos, sv_id, svid_len);
	pos += svid_len;

	/* smpCnt */
	buf[pos++] = 0x82;
	buf[pos++] = 0x02;
	buf[pos++] = (uint8_t)(smp_cnt >> 8);
	buf[pos++] = (uint8_t)(smp_cnt);

	/* Fill in SV PDU length (everything from appID to end) */
	uint16_t pdu_total_len = (uint16_t)(pos - sv_hdr_pos);
	buf[len_pos] = (uint8_t)(pdu_total_len >> 8);
	buf[len_pos + 1] = (uint8_t)(pdu_total_len);

	(void)buflen;
	return pos;
}

static void test_basic_parse(void)
{
	printf("  test_basic_parse...");
	uint8_t frame[256];
	size_t len = build_sv_frame(frame, sizeof(frame),
				    0x4000, "TestStream1", 42, 0);

	struct sv_frame_info info;
	int rc = sv_parse(frame, len, &info);
	assert(rc == 0);
	assert(info.app_id == 0x4000);
	assert(strcmp(info.sv_id, "TestStream1") == 0);
	assert(info.smp_cnt == 42);
	printf(" OK\n");
}

static void test_vlan_parse(void)
{
	printf("  test_vlan_parse...");
	uint8_t frame[256];
	size_t len = build_sv_frame(frame, sizeof(frame),
				    0x4001, "VlanStream", 1000, 1);

	struct sv_frame_info info;
	int rc = sv_parse(frame, len, &info);
	assert(rc == 0);
	assert(info.app_id == 0x4001);
	assert(strcmp(info.sv_id, "VlanStream") == 0);
	assert(info.smp_cnt == 1000);
	printf(" OK\n");
}

static void test_smp_cnt_high(void)
{
	printf("  test_smp_cnt_high...");
	uint8_t frame[256];
	size_t len = build_sv_frame(frame, sizeof(frame),
				    0x4002, "High", 65535, 0);

	struct sv_frame_info info;
	int rc = sv_parse(frame, len, &info);
	assert(rc == 0);
	assert(info.smp_cnt == 65535);
	printf(" OK\n");
}

static void test_wrong_ethertype(void)
{
	printf("  test_wrong_ethertype...");
	uint8_t frame[256];
	size_t len = build_sv_frame(frame, sizeof(frame),
				    0x4000, "Test", 1, 0);
	/* Corrupt EtherType */
	frame[12] = 0x08;
	frame[13] = 0x00;

	struct sv_frame_info info;
	int rc = sv_parse(frame, len, &info);
	assert(rc == -1);
	printf(" OK\n");
}

static void test_truncated(void)
{
	printf("  test_truncated...");
	uint8_t frame[10] = {0};
	struct sv_frame_info info;
	assert(sv_parse(frame, 10, &info) == -1);
	assert(sv_parse(NULL, 0, &info) == -1);
	printf(" OK\n");
}

int main(void)
{
	printf("=== SV Parser Tests ===\n");
	test_basic_parse();
	test_vlan_parse();
	test_smp_cnt_high();
	test_wrong_ethertype();
	test_truncated();
	printf("All SV parser tests passed.\n");
	return 0;
}
