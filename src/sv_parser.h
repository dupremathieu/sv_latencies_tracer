/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SV_PARSER_H
#define SV_PARSER_H

#include "common.h"

/*
 * Parse an SV Ethernet frame and extract appID, svID, and smpCnt.
 *
 * @param frame     Raw Ethernet frame (starting at dest MAC).
 * @param frame_len Length of the frame in bytes.
 * @param info      Output: parsed SV fields.
 * @return 0 on success, -1 on parse error.
 *
 * Handles both plain (EtherType at offset 12) and 802.1Q VLAN-tagged frames
 * (EtherType 0x8100 at offset 12, actual EtherType at offset 16).
 */
int sv_parse(const uint8_t *frame, size_t frame_len,
	     struct sv_frame_info *info);

#endif /* SV_PARSER_H */
