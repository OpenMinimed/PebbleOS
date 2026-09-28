// Minimal AES-128 (encrypt + decrypt of a single 16-byte block).
// Self-contained, no dynamic allocation, portable C99 — suitable for both the
// host verification harness and the nRF52840 firmware (where it can later be
// swapped for the hardware AES if desired).
#ifndef MINIMED_SAKE_AES_H
#define MINIMED_SAKE_AES_H

#include <stdint.h>

// Expanded key schedule for AES-128 (11 round keys).
typedef struct {
  uint8_t round_keys[176];
} sake_aes_ctx;

void sake_aes_init(sake_aes_ctx *ctx, const uint8_t key[16]);

// Encrypt / decrypt one 16-byte block in place-safe manner (in may equal out).
void sake_aes_encrypt_block(const sake_aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]);
void sake_aes_decrypt_block(const sake_aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]);

#endif  // MINIMED_SAKE_AES_H
