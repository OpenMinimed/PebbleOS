/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_iob.h"

// IDD SRCP "Get Insulin On Board" response opcode (Documentation/idd-service.md; upstream
// IddInsulinOnBoard RESPONSE_OPCODE). Little-endian on the wire: bytes fc 03.
#define IOB_RESPONSE_OPCODE 0x03FC
// Mandatory prefix: opcode(2) + flags(1) + IOB medfloat32(4).
#define IOB_MIN_BODY 7
// Plausibility ceiling: 100 IU = 100000 mU (upstream IddStatusReader MAX_IOB_UNITS). Real IOB
// rarely exceeds the low tens of IU; a value above this is a misaligned/garbled read, rejected.
#define IOB_MAX_MU 100000

bool minimed_iob_decode_medfloat32_mu(uint32_t raw, int32_t *out_mu) {
  int exponent = (int)((raw >> 24) & 0xFFu);
  int32_t mantissa = (int32_t)(raw & 0x00FFFFFFu);
  if (exponent & 0x80) exponent -= 0x100;          // sign-extend the 8-bit exponent
  if (mantissa & 0x800000) mantissa -= 0x1000000;  // sign-extend the 24-bit mantissa

  int p = exponent + 3;  // fold the *1000 (IU -> mU) into the power of ten
  int64_t v = mantissa;
  if (p >= 0) {
    for (int i = 0; i < p; i++) {
      v *= 10;
      if (v > INT32_MAX || v < INT32_MIN) return false;  // not representable as int32 mU
    }
  } else {
    int64_t div = 1;
    for (int i = 0; i < -p; i++) div *= 10;
    v = (v >= 0) ? (v + div / 2) / div : (v - div / 2) / div;  // round half away from zero
  }
  *out_mu = (int32_t)v;
  return true;
}

bool minimed_iob_parse_response(const uint8_t *body, uint16_t len, int32_t *out_mu) {
  if (len < IOB_MIN_BODY) return false;
  uint16_t opcode = (uint16_t)(body[0] | (body[1] << 8));  // little-endian
  if (opcode != IOB_RESPONSE_OPCODE) return false;
  // body[2] = flags (not needed for the value). IOB medfloat32 at offset 3, little-endian.
  uint32_t raw = (uint32_t)body[3] | ((uint32_t)body[4] << 8) | ((uint32_t)body[5] << 16) |
                 ((uint32_t)body[6] << 24);
  int32_t mu;
  if (!minimed_iob_decode_medfloat32_mu(raw, &mu)) return false;
  if (mu < 0 || mu > IOB_MAX_MU) return false;  // 0 .. 100 IU plausibility gate
  *out_mu = mu;
  return true;
}
