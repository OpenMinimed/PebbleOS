#include "minimed_sake_crypto.h"

#include <string.h>

#include "minimed_sake_aes.h"

// ---------------------------------------------------------------------------
// AES-CMAC (RFC 4493), truncated to mac_len bytes.
// ---------------------------------------------------------------------------

static void shl_block(const uint8_t in[16], uint8_t out[16]) {
  uint8_t carry = 0;
  for (int i = 15; i >= 0; i--) {
    uint8_t b = in[i];
    out[i] = (uint8_t)((b << 1) | carry);
    carry = (b >> 7) & 1;
  }
}

static void sake_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
                      uint8_t *out, size_t mac_len) {
  sake_aes_ctx ctx;
  sake_aes_init(&ctx, key);

  uint8_t l[16] = {0};
  sake_aes_encrypt_block(&ctx, l, l);

  uint8_t k1[16], k2[16];
  shl_block(l, k1);
  if (l[0] & 0x80) k1[15] ^= 0x87;
  shl_block(k1, k2);
  if (k1[0] & 0x80) k2[15] ^= 0x87;

  size_t n = (len + 15) / 16;
  bool complete;
  if (n == 0) {
    n = 1;
    complete = false;
  } else {
    complete = (len % 16) == 0;
  }

  uint8_t last[16];
  const uint8_t *last_src = msg + (n - 1) * 16;
  if (complete) {
    for (int i = 0; i < 16; i++) last[i] = last_src[i] ^ k1[i];
  } else {
    size_t rem = len % 16;
    for (size_t i = 0; i < 16; i++) {
      uint8_t b;
      if (i < rem) b = last_src[i];
      else if (i == rem) b = 0x80;
      else b = 0x00;
      last[i] = b ^ k2[i];
    }
  }

  uint8_t x[16] = {0};
  for (size_t i = 0; i < n - 1; i++) {
    for (int j = 0; j < 16; j++) x[j] ^= msg[i * 16 + j];
    sake_aes_encrypt_block(&ctx, x, x);
  }
  for (int j = 0; j < 16; j++) x[j] ^= last[j];
  sake_aes_encrypt_block(&ctx, x, x);

  memcpy(out, x, mac_len);
}

// ---------------------------------------------------------------------------
// AES-CTR keystream over `len` bytes. Counter block = nonce13 || 3-byte BE ctr.
// (pycryptodome MODE_CTR with a 13-byte nonce; matches SeqCrypt.)
// ---------------------------------------------------------------------------

static void sake_aes_ctr(const uint8_t key[16], const uint8_t nonce13[13],
                         const uint8_t *in, uint8_t *out, size_t len) {
  sake_aes_ctx ctx;
  sake_aes_init(&ctx, key);
  uint8_t block[16];
  uint8_t ks[16];
  memcpy(block, nonce13, 13);
  uint32_t counter = 0;
  size_t off = 0;
  while (off < len) {
    block[13] = (uint8_t)(counter >> 16);
    block[14] = (uint8_t)(counter >> 8);
    block[15] = (uint8_t)counter;
    sake_aes_encrypt_block(&ctx, block, ks);
    size_t chunk = len - off < 16 ? len - off : 16;
    for (size_t i = 0; i < chunk; i++) out[off + i] = in[off + i] ^ ks[i];
    off += chunk;
    counter++;
  }
}

// ---------------------------------------------------------------------------
// SeqCrypt: sequenced AES-CTR payload with a 2-on-wire / 4-internal-byte CMAC.
// ---------------------------------------------------------------------------

// Build the 13-byte CTR nonce: 5-byte BE seq || 8-byte static nonce.
static void seqcrypt_nonce(const sake_seqcrypt *sc, uint64_t seq, uint8_t nonce13[13]) {
  nonce13[0] = (uint8_t)(seq >> 32);
  nonce13[1] = (uint8_t)(seq >> 24);
  nonce13[2] = (uint8_t)(seq >> 16);
  nonce13[3] = (uint8_t)(seq >> 8);
  nonce13[4] = (uint8_t)seq;
  memcpy(nonce13 + 5, sc->nonce, 8);
}

// CMAC-4 over (nonce.ljust(16) || ciphertext).
static void seqcrypt_mac(const sake_seqcrypt *sc, const uint8_t nonce13[13],
                         const uint8_t *ct, size_t ct_len, uint8_t mac4[4]) {
  uint8_t buf[16 + SAKE_MSG_SIZE];  // padded nonce + up to a 20-byte frame body
  memcpy(buf, nonce13, 13);
  buf[13] = buf[14] = buf[15] = 0;
  memcpy(buf + 16, ct, ct_len);
  sake_cmac(sc->key, buf, 16 + ct_len, mac4, 4);
}

static void seqcrypt_encrypt(sake_seqcrypt *sc, const uint8_t *pt, size_t n, uint8_t *out) {
  uint64_t seq = sc->seq;
  uint8_t nonce13[13];
  seqcrypt_nonce(sc, seq, nonce13);
  sake_aes_ctr(sc->key, nonce13, pt, out, n);
  uint8_t mac[4];
  seqcrypt_mac(sc, nonce13, out, n, mac);
  out[n] = (uint8_t)((seq / 2) & 0xFF);
  out[n + 1] = mac[0];
  out[n + 2] = mac[1];
  sc->seq = seq + 2;
}

static bool seqcrypt_decrypt(sake_seqcrypt *sc, const uint8_t *msg, size_t m,
                             uint8_t *out, size_t *out_len) {
  if (m < 3) return false;
  size_t ct_len = m - 3;
  uint8_t d = (uint8_t)(msg[m - 3] - (sc->seq / 2));
  uint64_t seq = sc->seq + 2 * (uint64_t)d;
  uint8_t nonce13[13];
  seqcrypt_nonce(sc, seq, nonce13);
  uint8_t mac[4];
  seqcrypt_mac(sc, nonce13, msg, ct_len, mac);
  if (mac[0] != msg[m - 2] || mac[1] != msg[m - 1]) return false;
  sc->seq = seq + 2;
  sake_aes_ctr(sc->key, nonce13, msg, out, ct_len);
  *out_len = ct_len;
  return true;
}

// ---------------------------------------------------------------------------
// Key database.
// ---------------------------------------------------------------------------

static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

bool sake_keydb_parse(sake_keydb *db, const uint8_t *data, size_t len) {
  if (len < 6) return false;
  uint32_t want = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                  ((uint32_t)data[2] << 8) | data[3];
  if (crc32(data + 4, len - 4) != want) return false;
  uint8_t n = data[5];
  if (n == 0 || n > SAKE_MAX_REMOTES) return false;
  if (len != (size_t)(6 + 81 * n)) return false;
  db->local_device_type = data[4];
  db->n_remotes = n;
  for (uint8_t i = 0; i < n; i++) {
    size_t p = 6 + 81 * (size_t)i;
    db->remote_device_type[i] = data[p];
    const uint8_t *k = data + p + 1;
    memcpy(db->remote_keys[i].derivation_key, k + 0, 16);
    memcpy(db->remote_keys[i].handshake_auth_key, k + 16, 16);
    memcpy(db->remote_keys[i].permit_decrypt_key, k + 32, 16);
    memcpy(db->remote_keys[i].permit_auth_key, k + 48, 16);
    memcpy(db->remote_keys[i].handshake_payload, k + 64, 16);
  }
  return true;
}

static const sake_static_keys *keydb_lookup(const sake_keydb *db, uint8_t dev) {
  for (uint8_t i = 0; i < db->n_remotes; i++) {
    if (db->remote_device_type[i] == dev) return &db->remote_keys[i];
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Handshake CMACs (Documentation/sake.md).
// ---------------------------------------------------------------------------

// CMAC-8(handshake_auth_key, server_key_material || client_key_material || derivation_key).
static void handshake_auth8(const sake_server *s, uint8_t out[8]) {
  uint8_t msg[32];
  memcpy(msg + 0, s->server_key_material, 8);
  memcpy(msg + 8, s->client_key_material, 8);
  memcpy(msg + 16, s->derivation_key, 16);
  sake_cmac(s->handshake_auth_key, msg, 32, out, 8);
}

static void create_crypts(sake_server *s) {
  sake_aes_ctx ctx;
  sake_aes_init(&ctx, s->derivation_key);
  uint8_t material[16];
  memcpy(material + 0, s->server_key_material, 8);
  memcpy(material + 8, s->client_key_material, 8);
  uint8_t key[16];
  sake_aes_encrypt_block(&ctx, material, key);

  uint8_t nonce[8];
  memcpy(nonce + 0, s->client_nonce, 4);
  memcpy(nonce + 4, s->server_nonce, 4);

  memcpy(s->client_crypt.key, key, 16);
  memcpy(s->client_crypt.nonce, nonce, 8);
  s->client_crypt.seq = 0;
  memcpy(s->server_crypt.key, key, 16);
  memcpy(s->server_crypt.nonce, nonce, 8);
  s->server_crypt.seq = 1;
}

// Decrypt + authenticate a 16-byte permit with the server's static keys.
static bool check_permit(const sake_static_keys *keys, const uint8_t permit[16],
                         uint8_t prover_device_type) {
  sake_aes_ctx ctx;
  sake_aes_init(&ctx, keys->permit_decrypt_key);
  uint8_t plain[16];
  sake_aes_decrypt_block(&ctx, permit, plain);
  uint8_t mac[4];
  sake_cmac(keys->permit_auth_key, plain, 12, mac, 4);
  if (memcmp(mac, plain + 12, 4) != 0) return false;
  return plain[0] == 0 && plain[1] == prover_device_type;
}

// ---------------------------------------------------------------------------
// Server state machine.
// ---------------------------------------------------------------------------

void sake_server_init(sake_server *s, const sake_keydb *db,
                      uint8_t local_device_type, sake_rng_fn rng, void *rng_ud) {
  memset(s, 0, sizeof(*s));
  s->db = db;
  s->local_device_type = local_device_type;
  s->rng = rng;
  s->rng_ud = rng_ud;
  s->stage = 0;
}

sake_result sake_server_handshake(sake_server *s, const uint8_t in[SAKE_MSG_SIZE],
                                  uint8_t out[SAKE_MSG_SIZE]) {
  switch (s->stage) {
    case 0: {
      for (int i = 0; i < SAKE_MSG_SIZE; i++) {
        if (in[i] != 0) return SAKE_RESULT_ERR;  // must start with 20 zero bytes
      }
      out[0] = s->local_device_type;
      out[1] = 0x01;
      s->rng(s->rng_ud, out + 2, 18);
      s->stage = 1;
      return SAKE_RESULT_MSG;
    }
    case 1: {
      memcpy(s->client_key_material, in + 0, 8);
      s->client_device_type = in[8];
      memcpy(s->client_nonce, in + 9, 4);

      const sake_static_keys *keys = keydb_lookup(s->db, s->client_device_type);
      if (!keys) return SAKE_RESULT_ERR;
      s->keys = keys;
      memcpy(s->derivation_key, keys->derivation_key, 16);
      memcpy(s->handshake_auth_key, keys->handshake_auth_key, 16);

      s->rng(s->rng_ud, s->server_key_material, 8);
      s->rng(s->rng_ud, s->server_nonce, 4);

      uint8_t auth[8];
      handshake_auth8(s, auth);
      memcpy(out + 0, auth, 8);
      memcpy(out + 8, s->server_key_material, 8);
      memcpy(out + 16, s->server_nonce, 4);
      s->stage = 3;
      return SAKE_RESULT_MSG;
    }
    case 3: {
      // Verify the client's mutual-auth tag: CMAC-8 over
      // auth1 || server_key_material || derivation_key, keyed by handshake_auth_key.
      uint8_t auth1[8];
      handshake_auth8(s, auth1);
      uint8_t inner[32];
      memcpy(inner + 0, auth1, 8);
      memcpy(inner + 8, s->server_key_material, 8);
      memcpy(inner + 16, s->derivation_key, 16);
      uint8_t expected[8];
      sake_cmac(s->handshake_auth_key, inner, 32, expected, 8);
      if (memcmp(expected, in, 8) != 0) return SAKE_RESULT_ERR;

      create_crypts(s);

      // Build msg4: session-encrypt (handshake_payload || 1 pad byte).
      uint8_t pt[17];
      memcpy(pt, s->keys->handshake_payload, 16);
      s->rng(s->rng_ud, pt + 16, 1);  // pad byte (value irrelevant to the peer)
      seqcrypt_encrypt(&s->server_crypt, pt, 17, out);
      s->stage = 5;
      return SAKE_RESULT_MSG;
    }
    case 5: {
      uint8_t pt[SAKE_MSG_SIZE];
      size_t pt_len = 0;
      if (!seqcrypt_decrypt(&s->client_crypt, in, SAKE_MSG_SIZE, pt, &pt_len)) {
        return SAKE_RESULT_ERR;
      }
      if (pt_len < 17) return SAKE_RESULT_ERR;
      // inner = plaintext without the trailing pad byte (16-byte permit).
      if (!check_permit(s->keys, pt, s->client_device_type)) return SAKE_RESULT_ERR;
      s->stage = 6;
      return SAKE_RESULT_DONE;
    }
    default:
      return SAKE_RESULT_ERR;
  }
}

void sake_encrypt_for_pump(sake_server *s, const uint8_t *pt, size_t n, uint8_t *out) {
  seqcrypt_encrypt(&s->server_crypt, pt, n, out);
}

bool sake_decrypt_from_pump(sake_server *s, const uint8_t *ct, size_t n,
                            uint8_t *out, size_t *out_len) {
  return seqcrypt_decrypt(&s->client_crypt, ct, n, out, out_len);
}

#ifdef SAKE_TEST_HOOKS
// Expose internals to the host KAT harness only.
void sake_cmac_test(const uint8_t key[16], const uint8_t *msg, size_t len,
                    uint8_t *out, size_t mac_len) {
  sake_cmac(key, msg, len, out, mac_len);
}
void sake_seqcrypt_encrypt_test(sake_seqcrypt *sc, const uint8_t *pt, size_t n, uint8_t *out) {
  seqcrypt_encrypt(sc, pt, n, out);
}
bool sake_seqcrypt_decrypt_test(sake_seqcrypt *sc, const uint8_t *msg, size_t m,
                                uint8_t *out, size_t *out_len) {
  return seqcrypt_decrypt(sc, msg, m, out, out_len);
}
#endif
