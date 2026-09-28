/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_idd_flags.h"

// Continuation-bit positions (bit 15 of each block); never real flags.
#define STRUCTURAL_BITS ((1ULL << 15) | (1ULL << 31) | (1ULL << 47))

uint64_t minimed_idd_flags_parse(const uint8_t *plain, uint16_t len) {
  uint64_t flags = 0;
  unsigned blocks = 0;
  for (unsigned i = 0; i + 1 < len && blocks < 3; i += 2) {
    const uint16_t block = (uint16_t)(plain[i] | (plain[i + 1] << 8));
    flags |= (uint64_t)block << (16 * blocks);
    blocks++;
    if ((block & 0x8000) == 0) {
      break;  // no continuation bit: this was the last block
    }
  }
  return flags;
}

uint16_t minimed_idd_flags_encode(uint64_t flags, uint8_t out[6]) {
  const uint64_t real = flags & ~STRUCTURAL_BITS;
  unsigned blocks = 1;
  if (real >> 32) {
    blocks = 3;
  } else if (real >> 16) {
    blocks = 2;
  }
  for (unsigned b = 0; b < blocks; b++) {
    uint16_t word = (uint16_t)(real >> (16 * b)) & 0x7FFF;
    if (b + 1 < blocks) word |= 0x8000;  // another block follows
    out[2 * b] = (uint8_t)word;
    out[2 * b + 1] = (uint8_t)(word >> 8);
  }
  return (uint16_t)(2 * blocks);
}
