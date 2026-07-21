// SAKE server-role handshake, ported from pysake (OpenMinimed, GPL-3.0).
//
// The watch plays the SAKE *server* (MOBILE_APPLICATION); the pump is the
// client (INSULIN_PUMP). Drive it from the SAKE-port GATT write handler:
// feed each 20-byte pump write into sake_server_handshake() and notify the
// 20-byte reply back. See PebbleOS/SPIKE-HANDOFF.md and Documentation/sake.md.
#ifndef MINIMED_SAKE_CRYPTO_H
#define MINIMED_SAKE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SAKE_MSG_SIZE 20

// Device types (Documentation/key_databases.md / pysake device_types.py).
enum {
  SAKE_DEV_INSULIN_PUMP = 0x1,
  SAKE_DEV_GLUCOSE_SENSOR = 0x2,
  SAKE_DEV_MOBILE_APPLICATION = 0x4,
};

typedef struct {
  uint8_t derivation_key[16];
  uint8_t handshake_auth_key[16];
  uint8_t permit_decrypt_key[16];
  uint8_t permit_auth_key[16];
  uint8_t handshake_payload[16];
} sake_static_keys;

#define SAKE_MAX_REMOTES 4

// Parsed key database. Layout: [4B CRC32-BE][1B local_dev][1B n][n * (1B dev + 80B keys)].
typedef struct {
  uint8_t local_device_type;
  uint8_t n_remotes;
  uint8_t remote_device_type[SAKE_MAX_REMOTES];
  sake_static_keys remote_keys[SAKE_MAX_REMOTES];
} sake_keydb;

// Parse + CRC-validate a key database. Returns false on bad CRC / bad length.
bool sake_keydb_parse(sake_keydb *db, const uint8_t *data, size_t len);

// Fills `out` with `n` random bytes. Must be a real CSPRNG in production;
// the host test injects a fixed queue to reproduce a captured trace.
typedef void (*sake_rng_fn)(void *ud, uint8_t *out, size_t n);

typedef struct {
  uint8_t key[16];
  uint8_t nonce[8];
  uint64_t seq;
} sake_seqcrypt;

typedef struct {
  const sake_keydb *db;
  uint8_t local_device_type;  // server device type (MOBILE_APPLICATION)
  int stage;                  // 0 -> 1 -> 3 -> 5 -> 6 (complete)

  uint8_t client_key_material[8];
  uint8_t client_nonce[4];
  uint8_t client_device_type;
  uint8_t server_key_material[8];
  uint8_t server_nonce[4];

  const sake_static_keys *keys;  // static keys selected for the client peer
  uint8_t derivation_key[16];
  uint8_t handshake_auth_key[16];

  sake_seqcrypt client_crypt;  // pump -> watch traffic
  sake_seqcrypt server_crypt;  // watch -> pump traffic

  sake_rng_fn rng;
  void *rng_ud;
} sake_server;

typedef enum {
  SAKE_RESULT_MSG,   // *out holds a 20-byte reply to notify to the pump
  SAKE_RESULT_DONE,  // handshake complete; session ciphers ready, nothing to send
  SAKE_RESULT_ERR,   // protocol / auth failure
} sake_result;

void sake_server_init(sake_server *s, const sake_keydb *db,
                      uint8_t local_device_type, sake_rng_fn rng, void *rng_ud);

// Advance the handshake with a 20-byte pump write; the pump's first write is
// 20 zero bytes. On SAKE_RESULT_MSG, `out` (20 bytes) must be sent back.
sake_result sake_server_handshake(sake_server *s, const uint8_t in[SAKE_MSG_SIZE],
                                  uint8_t out[SAKE_MSG_SIZE]);

static inline bool sake_server_is_complete(const sake_server *s) { return s->stage == 6; }

// Post-handshake session ciphers (only valid once complete). `out` must hold
// at least plaintext_len + 3 bytes for encrypt. Decrypt writes ciphertext_len - 3.
void sake_encrypt_for_pump(sake_server *s, const uint8_t *pt, size_t n, uint8_t *out);
bool sake_decrypt_from_pump(sake_server *s, const uint8_t *ct, size_t n,
                            uint8_t *out, size_t *out_len);

#endif  // MINIMED_SAKE_CRYPTO_H
