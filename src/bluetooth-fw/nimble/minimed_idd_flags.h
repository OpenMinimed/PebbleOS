/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Pure parse/encode of the Medtronic IDD Status Changed flag field (0x101; also the operand of
//! SRCP Reset Status 0x030C). Self-extending little-endian 16-bit blocks: bit 15/31 of a block
//! set means another block follows, so the field is 16/32/48 bits wide
//! (Documentation/idd-service.md). Kept free of any NimBLE/firmware dependency so the host test
//! harness (tools/minimed_sake_hosttest) can verify it -- pattern: minimed_iob.{c,h}.

//! Bits acted on by the read path (Documentation/idd-service.md, same set the bridge uses).
#define MINIMED_IDD_FLAG_IOB (1ULL << 17)
#define MINIMED_IDD_FLAG_NEW_CGM (1ULL << 18)

//! Decode a decrypted flag field. Returns the flag word INCLUDING the continuation bits
//! (15/31), matching the bridge, which echoes the raw field back to Reset Status verbatim.
//! Stops at the first block whose continuation bit is clear, at 3 blocks, or when the buffer
//! runs out of whole blocks; returns 0 for a buffer shorter than one block.
uint64_t minimed_idd_flags_parse(const uint8_t *plain, uint16_t len);

//! Inverse: emit `flags` as 1-3 LE 16-bit blocks into out[0..5], returning the byte count
//! (2/4/6). Width = the highest set REAL bit; continuation bits are recomputed (set on every
//! non-final block, clear on the final one), so a union of observations with different widths
//! encodes correctly. The structural positions (15, 31, 47) in the input are ignored.
uint16_t minimed_idd_flags_encode(uint64_t flags, uint8_t out[6]);
