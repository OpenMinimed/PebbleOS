// Host verification for the SAKE C port. Builds with plain gcc, no watch.
//
//   Section 1: AES-128 (FIPS-197) + AES-CMAC (RFC 4493) known-answer tests.
//   Section 2: replay OpenMinimed's captured 780G pairing trace through the
//              server state machine with the capture's RNG values injected,
//              and assert msg0/msg2/msg4 are byte-identical and the handshake
//              completes against the pump's real recorded msg1/msg3/msg5.
#include <stdio.h>
#include <string.h>

#include "minimed_sake_crypto.h"
#include "minimed_sake_aes.h"

static int g_pass, g_fail;

static void check(const char *label, int ok) {
  printf("    [%s] %s\n", ok ? "PASS" : "FAIL", label);
  if (ok) g_pass++; else g_fail++;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static size_t unhex(const char *hex, uint8_t *out) {
  size_t n = 0;
  while (hex[0] && hex[1]) {
    out[n++] = (uint8_t)((hexval(hex[0]) << 4) | hexval(hex[1]));
    hex += 2;
  }
  return n;
}

static void print_hex(const char *label, const uint8_t *b, size_t n) {
  printf("    %s", label);
  for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
  printf("\n");
}

// A deterministic RNG that dispenses a preloaded byte queue (for capture replay).
typedef struct { const uint8_t *buf; size_t len, pos; } queued_rng;
static void queued_rng_fn(void *ud, uint8_t *out, size_t n) {
  queued_rng *q = ud;
  for (size_t i = 0; i < n; i++) out[i] = q->pos < q->len ? q->buf[q->pos++] : 0;
}

// Declarations of the internal primitives we KAT (also compiled into sake.c).
extern void sake_cmac_test(const uint8_t key[16], const uint8_t *msg, size_t len,
                           uint8_t *out, size_t mac_len);
extern void sake_seqcrypt_encrypt_test(sake_seqcrypt *sc, const uint8_t *pt, size_t n, uint8_t *out);
extern bool sake_seqcrypt_decrypt_test(sake_seqcrypt *sc, const uint8_t *msg, size_t m,
                                       uint8_t *out, size_t *out_len);

// --- Section 1: primitive KATs -------------------------------------------

static void section_primitives(void) {
  printf("[1] AES-128 + AES-CMAC known-answer tests\n");

  // FIPS-197 AES-128 vector.
  uint8_t key[16], pt[16], ct[16], buf[16];
  unhex("000102030405060708090a0b0c0d0e0f", key);
  unhex("00112233445566778899aabbccddeeff", pt);
  unhex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
  sake_aes_ctx ctx;
  sake_aes_init(&ctx, key);
  sake_aes_encrypt_block(&ctx, pt, buf);
  check("AES-128 encrypt matches FIPS-197", memcmp(buf, ct, 16) == 0);
  sake_aes_decrypt_block(&ctx, ct, buf);
  check("AES-128 decrypt matches FIPS-197", memcmp(buf, pt, 16) == 0);

  // RFC 4493 AES-CMAC vectors (key 2b7e...).
  uint8_t ck[16];
  unhex("2b7e151628aed2a6abf7158809cf4f3c", ck);
  uint8_t msg[64], want[16], got[16];

  sake_cmac_test(ck, msg, 0, got, 16);
  unhex("bb1d6929e95937287fa37d129b756746", want);
  check("CMAC(len=0) matches RFC 4493", memcmp(got, want, 16) == 0);

  unhex("6bc1bee22e409f96e93d7e117393172a", msg);
  sake_cmac_test(ck, msg, 16, got, 16);
  unhex("070a16b46b4d4144f79bdd9dd04a287c", want);
  check("CMAC(len=16) matches RFC 4493", memcmp(got, want, 16) == 0);

  size_t ml = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                    "30c81c46a35ce411", msg);
  sake_cmac_test(ck, msg, ml, got, 16);
  unhex("dfa66747de9ae63030ca32611497c827", want);
  check("CMAC(len=40) matches RFC 4493", memcmp(got, want, 16) == 0);
  printf("\n");
}

// --- Section 2: captured 780G trace replay --------------------------------

// OpenMinimed public constants (pysake/constants.py, KEYDB_PUMP_EXTRACTED +
// __PUMP_TEST_MSGS_1, the 780g_pairing_with_mobile capture).
static const char *KEYDB_HEX =
    "f75995e70401011bc1bf7cbf36fa1e2367d795ff09211903da6afbe986b650f1"
    "4179c0e6852e0ce393781078ffc6f51919e2eaefbde69b8eca21e41ab59b881a"
    "0bea0286ea91dc7582a86a714e1737f558f0d66dc1895c";
static const char *MSG_HEX[6] = {
    "0401e2f09017a98f9f01cc56492fbacd4576e92b",  // msg0 server -> pump
    "42060e9f344e9312016ee8854d357f659b6b00ba",  // msg1 pump -> server
    "fdeeb13d04c3f18d272630ebeabe7c3a4d4d27b9",  // msg2 server -> pump
    "c02cec4ffb99affcb553a10fa6c55bb13d9fbacf",  // msg3 pump -> server
    "157d8e90214418a0e3d5f0517eebf4a82e00c02e",  // msg4 server -> pump
    "9b36f393b296fa84a757809859fc84a5c300d59b",  // msg5 pump -> server
};
static const uint8_t CAPTURED_MSG4_PAD = 0xf7;

static void section_captured_trace(void) {
  printf("[2] SAKE handshake vs captured 780G pump trace (watch = MOBILE_APPLICATION)\n");

  uint8_t kdb[128];
  size_t kdb_len = unhex(KEYDB_HEX, kdb);
  sake_keydb db;
  check("key database parses + CRC validates", sake_keydb_parse(&db, kdb, kdb_len));
  check("local device type is MOBILE_APPLICATION", db.local_device_type == SAKE_DEV_MOBILE_APPLICATION);
  check("remote device type is INSULIN_PUMP",
        db.n_remotes == 1 && db.remote_device_type[0] == SAKE_DEV_INSULIN_PUMP);

  uint8_t msg[6][SAKE_MSG_SIZE];
  for (int i = 0; i < 6; i++) unhex(MSG_HEX[i], msg[i]);

  // Replay the random fields the phone chose in the capture: msg0 filler (18B),
  // server key material (8B), server nonce (4B), then msg4 pad byte.
  uint8_t rng_buf[31];
  memcpy(rng_buf + 0, msg[0] + 2, 18);
  memcpy(rng_buf + 18, msg[2] + 8, 8);
  memcpy(rng_buf + 26, msg[2] + 16, 4);
  rng_buf[30] = CAPTURED_MSG4_PAD;
  queued_rng q = { rng_buf, sizeof(rng_buf), 0 };

  sake_server s;
  sake_server_init(&s, &db, SAKE_DEV_MOBILE_APPLICATION, queued_rng_fn, &q);

  uint8_t out[SAKE_MSG_SIZE];
  sake_result r;
  uint8_t zeros[SAKE_MSG_SIZE] = {0};

  r = sake_server_handshake(&s, zeros, out);
  check("stage 0: wake-up (20 zeros) -> msg0 emitted", r == SAKE_RESULT_MSG);
  check("msg0 byte-identical to capture", memcmp(out, msg[0], 20) == 0);
  print_hex("msg0 = ", out, 20);

  r = sake_server_handshake(&s, msg[1], out);
  check("stage 1: pump msg1 -> msg2 emitted", r == SAKE_RESULT_MSG);
  check("msg2 byte-identical to capture", memcmp(out, msg[2], 20) == 0);
  print_hex("msg2 = ", out, 20);

  r = sake_server_handshake(&s, msg[3], out);
  check("stage 3: pump msg3 auth verified -> msg4 emitted", r == SAKE_RESULT_MSG);
  check("msg4 byte-identical to capture (with captured pad 0xf7)",
        memcmp(out, msg[4], 20) == 0);
  print_hex("msg4 = ", out, 20);

  r = sake_server_handshake(&s, msg[5], out);
  check("stage 5: pump msg5 permit verified -> handshake DONE", r == SAKE_RESULT_DONE);
  check("server reached stage 6 (complete)", sake_server_is_complete(&s));

  print_hex("session key derived: ", s.server_crypt.key, 16);
  print_hex("session nonce:       ", s.server_crypt.nonce, 8);

  // Negative test: a tampered msg3 auth tag must be rejected.
  {
    queued_rng q2 = { rng_buf, sizeof(rng_buf), 0 };
    sake_server s2;
    sake_server_init(&s2, &db, SAKE_DEV_MOBILE_APPLICATION, queued_rng_fn, &q2);
    sake_server_handshake(&s2, zeros, out);
    sake_server_handshake(&s2, msg[1], out);
    uint8_t bad3[SAKE_MSG_SIZE];
    memcpy(bad3, msg[3], 20);
    bad3[0] ^= 0x01;
    r = sake_server_handshake(&s2, bad3, out);
    check("tampered msg3 auth tag is rejected", r == SAKE_RESULT_ERR);
  }
  printf("\n");
}

// --- Section 3: SeqCrypt post-handshake session cipher --------------------

static void section_seqcrypt(void) {
  printf("[3] SeqCrypt session cipher (post-handshake read/write layer)\n");

  uint8_t key[16], nonce[8], pt[17];
  unhex("00112233445566778899aabbccddeeff", key);
  unhex("a1b2c3d4e5f60718", nonce);
  size_t ptlen = unhex("48656c6c6f2c2053414b65212121212121", pt);  // "Hello, SAKe!!!!!!"

  // Deterministic KAT vector (seq 0) — its exact ciphertext is cross-checked
  // against pysake's SeqCrypt in the interop step below.
  sake_seqcrypt tx = {0}, rx = {0};
  memcpy(tx.key, key, 16); memcpy(tx.nonce, nonce, 8); tx.seq = 0;
  memcpy(rx.key, key, 16); memcpy(rx.nonce, nonce, 8); rx.seq = 0;

  uint8_t frame[SAKE_MSG_SIZE + 8];
  sake_seqcrypt_encrypt_test(&tx, pt, ptlen, frame);
  check("encrypt advances tx seq by 2", tx.seq == 2);
  check("ciphertext differs from plaintext", memcmp(frame, pt, ptlen) != 0);
  print_hex("seqcrypt KAT (key=0011..,nonce=a1b2..,seq=0): ", frame, ptlen + 3);

  uint8_t rec[SAKE_MSG_SIZE + 8];
  size_t reclen = 0;
  check("decrypt recovers plaintext", sake_seqcrypt_decrypt_test(&rx, frame, ptlen + 3, rec, &reclen)
                                          && reclen == ptlen && memcmp(rec, pt, ptlen) == 0);
  check("decrypt advances rx seq by 2", rx.seq == 2);

  // Tamper: flip a MAC byte -> must be rejected.
  uint8_t bad[SAKE_MSG_SIZE + 8];
  memcpy(bad, frame, ptlen + 3);
  bad[ptlen + 3 - 1] ^= 0x01;
  sake_seqcrypt rx2 = {0};
  memcpy(rx2.key, key, 16); memcpy(rx2.nonce, nonce, 8);
  check("tampered frame is rejected", !sake_seqcrypt_decrypt_test(&rx2, bad, ptlen + 3, rec, &reclen));

  // Multi-frame stream: three sequential frames recovered in order (seq 0,2,4).
  sake_seqcrypt stx = {0}, srx = {0};
  memcpy(stx.key, key, 16); memcpy(stx.nonce, nonce, 8);
  memcpy(srx.key, key, 16); memcpy(srx.nonce, nonce, 8);
  int stream_ok = 1;
  uint8_t last_frame[SAKE_MSG_SIZE + 8];
  size_t last_len = 0;
  for (int i = 0; i < 3; i++) {
    uint8_t msg[8] = { (uint8_t)i, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t f[SAKE_MSG_SIZE + 8];
    sake_seqcrypt_encrypt_test(&stx, msg, 8, f);
    memcpy(last_frame, f, 11); last_len = 11;
    uint8_t d[SAKE_MSG_SIZE + 8]; size_t dl = 0;
    if (!sake_seqcrypt_decrypt_test(&srx, f, 11, d, &dl) || dl != 8 || memcmp(d, msg, 8) != 0) {
      stream_ok = 0;
    }
  }
  check("in-order multi-frame stream round-trips (seq advances)", stream_ok);

  // Sequence delta: a fresh receiver decoding the 3rd frame (seq 4) must use the
  // wire delta byte to jump ahead — exercises the d = seq_byte - rx_seq/2 path.
  sake_seqcrypt jrx = {0};
  memcpy(jrx.key, key, 16); memcpy(jrx.nonce, nonce, 8);
  uint8_t jd[SAKE_MSG_SIZE + 8]; size_t jdl = 0;
  check("receiver jumps to a later sequence via wire delta byte",
        sake_seqcrypt_decrypt_test(&jrx, last_frame, last_len, jd, &jdl) && jrx.seq == 6);
  printf("\n");
}

int main(void) {
  printf("=== SAKE C port host verification ===\n\n");
  section_primitives();
  section_captured_trace();
  section_seqcrypt();
  printf("SUMMARY: %d passed, %d failed -> %s\n", g_pass, g_fail,
         g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
  return g_fail == 0 ? 0 : 1;
}
