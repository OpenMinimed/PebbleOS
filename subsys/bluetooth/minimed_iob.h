/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Pure IOB (insulin-on-board) decode/parse for the MiniMed IDD SRCP path. Kept free of any
//! NimBLE/firmware dependency so the host test harness (tools/minimed_sake_hosttest) can link and
//! byte-verify it against the OpenMinimed vectors before it ever runs on hardware. Ported from the
//! HW-verified Kotlin (MedtronicCodec.decodeMedFloat32 + IddInsulinOnBoard.parse).

//! Decode a 32-bit IEEE-11073 FLOAT (medfloat32) to insulin milliunits (IU * 1000). `raw` is the
//! 4 medfloat32 bytes read little-endian as a uint32. value = mantissa(signed 24-bit) *
//! 10^exponent(signed 8-bit), scaled by 1000. Returns false only if the result is not
//! representable as int32 milliunits (technical overflow); it does NOT apply a plausibility gate
//! (medfloat32 has no NaN/Inf codes -- the range check lives in the caller/parse).
bool minimed_iob_decode_medfloat32_mu(uint32_t raw, int32_t *out_mu);

//! Parse an IDD "Get Insulin On Board" response body (SRCP response opcode 0x03FC). `body` is the
//! decrypted, reassembled plaintext (no E2E trailer on the 780G). Requires >= 7 bytes:
//! opcode(2,LE) + flags(1) + IOB medfloat32(4,LE). Writes IOB in milliunits to *out_mu. Returns
//! false on a short body, wrong opcode, an unrepresentable decode, or a value outside the
//! plausible 0..100 IU range (a garbled/misaligned read is rejected, never surfaced).
bool minimed_iob_parse_response(const uint8_t *body, uint16_t len, int32_t *out_mu);
