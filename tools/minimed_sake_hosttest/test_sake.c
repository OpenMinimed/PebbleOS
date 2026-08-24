// Host verification for the SAKE C port. Builds with plain gcc, no watch.
//
//   Section 1: AES-128 (FIPS-197) + AES-CMAC (RFC 4493) known-answer tests.
//   Section 2: replay OpenMinimed's captured 780G pairing trace through the
//              server state machine with the capture's RNG values injected,
//              and assert msg0/msg2/msg4 are byte-identical and the handshake
//              completes against the pump's real recorded msg1/msg3/msg5.
#include <stdio.h>
#include <string.h>

#include "minimed_annunciation.h"
#include "minimed_sake_crypto.h"
#include "minimed_sake_aes.h"
#include "minimed_graph.h"
#include "minimed_idd_flags.h"
#include "minimed_iob.h"
#include "minimed_status.h"

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

// --- Section 4: IOB medfloat32 decode + SRCP 0x03FC parse -----------------
// Vectors from OpenMinimed's Kotlin unit tests (MedtronicCodecMedFloat32Test / IddStatusReaderTest);
// the 1.4 IU frame is confirmed against a live 780G. This pins minimed_iob.c before it ever flashes.

static void section_iob(void) {
  printf("[4] IOB medfloat32 decode + SRCP 0x03FC parse\n");
  int32_t mu = 0;

  check("medfloat32 0xfa155cc0 -> 1400 mU (1.4 IU)",
        minimed_iob_decode_medfloat32_mu(0xfa155cc0u, &mu) && mu == 1400);
  check("medfloat32 0xfb280de8 -> 26250 mU (26.25)",
        minimed_iob_decode_medfloat32_mu(0xfb280de8u, &mu) && mu == 26250);
  check("medfloat32 0 -> 0 mU", minimed_iob_decode_medfloat32_mu(0u, &mu) && mu == 0);
  check("medfloat32 0x000000c8 -> 200000 mU (200 IU, decode is faithful)",
        minimed_iob_decode_medfloat32_mu(0x000000c8u, &mu) && mu == 200000);

  uint8_t body[16];
  size_t n = unhex("fc0300c05c15fa", body);  // live-confirmed IOB response = 1.4 IU
  check("parse IOB response 'fc0300c05c15fa' -> 1400 mU",
        minimed_iob_parse_response(body, (uint16_t)n, &mu) && mu == 1400);

  n = unhex("fc0300c8000000", body);  // 200 IU -> rejected by the 0..100 IU plausibility gate
  check("parse rejects out-of-range 200 IU", !minimed_iob_parse_response(body, (uint16_t)n, &mu));

  n = unhex("fb0300c05c15fa", body);  // wrong opcode (0x03fb)
  check("parse rejects wrong opcode", !minimed_iob_parse_response(body, (uint16_t)n, &mu));

  n = unhex("fc0300c05c15", body);  // 6 bytes < MIN_BODY_SIZE(7)
  check("parse rejects short body", !minimed_iob_parse_response(body, (uint16_t)n, &mu));
  printf("\n");
}

// --- Section 5: graph history buffer + wire encoding ----------------------
// The watch has no pump-side backfill, so the graph is whatever readings it has accumulated. The
// awkward cases are all about keeping the array strictly ascending (the offset-from-oldest wire
// format cannot express anything else) and aging points out of the window.

#define T0 1800000000u  // arbitrary epoch base for readable arithmetic
#define MIN(m) ((m) * 60u)

static uint16_t g_blob_len;
static uint8_t g_blob[MINIMED_GRAPH_BLOB_MAX];

static uint16_t blob_count(void) { return (uint16_t)(g_blob[4] | (g_blob[5] << 8)); }
static uint32_t blob_ref(void) {
  return (uint32_t)g_blob[0] | ((uint32_t)g_blob[1] << 8) | ((uint32_t)g_blob[2] << 16) |
         ((uint32_t)g_blob[3] << 24);
}
static uint16_t blob_offset(int i) {
  return (uint16_t)(g_blob[6 + 2 * i] | (g_blob[7 + 2 * i] << 8));
}
static uint8_t blob_bg(int i) { return g_blob[6 + 2 * blob_count() + i]; }

static void section_graph(void) {
  printf("[5] Graph history buffer + wire encoding\n");
  MinimedGraph g = {0};

  check("empty graph serializes to nothing", minimed_graph_serialize(&g, g_blob) == 0);

  minimed_graph_add(&g, T0, 100);
  minimed_graph_add(&g, T0 + MIN(5), 110);
  minimed_graph_add(&g, T0 + MIN(10), 121);
  g_blob_len = minimed_graph_serialize(&g, g_blob);
  check("3 points -> 6 + 3N bytes", g_blob_len == 6 + 3 * 3);
  check("count field is 3", blob_count() == 3);
  check("ref timestamp is the oldest point", blob_ref() == T0);
  check("offsets are minutes from ref", blob_offset(0) == 0 && blob_offset(1) == 5 &&
                                            blob_offset(2) == 10);
  // mg/dL / 2, rounded: 100 -> 50, 110 -> 55, 121 -> 61.
  check("bg values are mg/dL/2, rounded",
        blob_bg(0) == 50 && blob_bg(1) == 55 && blob_bg(2) == 61);

  check("negative reading is ignored",
        (minimed_graph_add(&g, T0 + MIN(15), -1), g.count == 3));
  check("out-of-range reading clamps to 255",
        (minimed_graph_add(&g, T0 + MIN(15), 900), g.bg[3] == 255));

  // A point exactly WINDOW old ages out; the window is a half-open interval.
  MinimedGraph w = {0};
  minimed_graph_add(&w, T0, 100);
  minimed_graph_add(&w, T0 + MINIMED_GRAPH_WINDOW_SECS, 120);
  check("point exactly one window old is dropped", w.count == 1 && w.bg[0] == 60);

  // Overflow: feed more points than the buffer holds. Spacing must be >= 1 min or the /60 in the
  // wire encoding collapses every offset to 0 and the ascending check below proves nothing --
  // 4 min keeps all 30 retained points inside the 150 min window, so eviction is by capacity.
  MinimedGraph f = {0};
  const int n_fill = MINIMED_GRAPH_MAX_POINTS + 10;
  for (int i = 0; i < n_fill; i++) {
    minimed_graph_add(&f, T0 + MIN(4 * i), (int32_t)(100 + i));
  }
  check("buffer caps at MAX_POINTS", f.count == MINIMED_GRAPH_MAX_POINTS);
  check("oldest points are the ones evicted", f.ts[0] == T0 + MIN(4 * 10));
  check("newest point is retained", f.ts[f.count - 1] == T0 + MIN(4 * (n_fill - 1)));

  // Clock stepping backwards (time sync / DST) must not produce an unsortable array.
  MinimedGraph b = {0};
  minimed_graph_add(&b, T0 + MIN(60), 100);
  minimed_graph_add(&b, T0 + MIN(65), 110);
  minimed_graph_add(&b, T0 + MIN(10), 120);  // jumped back an hour
  check("backwards clock discards the now-future points", b.count == 1 && b.ts[0] == T0 + MIN(10));

  // A duplicate timestamp would encode two points at the same x; treat it as a replacement.
  MinimedGraph d = {0};
  minimed_graph_add(&d, T0, 100);
  minimed_graph_add(&d, T0, 140);
  check("duplicate timestamp replaces rather than duplicates", d.count == 1 && d.bg[0] == 70);

  // Serialized offsets must stay ascending across a full buffer -- this is what the watchface
  // relies on to draw a left-to-right trace.
  g_blob_len = minimed_graph_serialize(&f, g_blob);
  // Guard the guard: with sub-minute spacing every offset encodes to 0 and the ascending check
  // below can't fail for any implementation. Assert the offsets actually differ first.
  check("fill spacing yields distinct offsets", blob_offset(1) > blob_offset(0));
  int ascending = 1;
  for (int i = 1; i < blob_count(); i++) {
    if (blob_offset(i) <= blob_offset(i - 1)) ascending = 0;
  }
  check("full-buffer offsets are strictly ascending", ascending);
  check("full-buffer blob length matches count",
        g_blob_len == 6 + 3 * MINIMED_GRAPH_MAX_POINTS);
  printf("\n");
}

// --- Section 6: IDD Status Changed flags (parse + Reset Status encode) -----
// The 0x101 flag field is self-extending: LE 16-bit blocks, bit 15/31 of a block = "another
// block follows" (Documentation/idd-service.md). parse returns the raw word INCLUDING the
// continuation bits (the bridge echoes them back to Reset Status verbatim); encode recomputes
// them from the width, because pending resets accumulate as a union whose observations may have
// had different widths.

static void section_idd_flags(void) {
  printf("--- IDD Status Changed flags ---\n");
  uint8_t out[6];

  // The vector observed on this pump overnight 2026-07-26/27 (PROGRESS.md item 3b).
  const uint8_t observed[] = {0xef, 0x81, 0x4f, 0x00};
  check("parse of observed HW vector ef814f00",
        minimed_idd_flags_parse(observed, sizeof(observed)) == 0x004f81efULL);

  const uint8_t one_block[] = {0x08, 0x00};
  check("parse 16-bit block (bit 3 only)",
        minimed_idd_flags_parse(one_block, sizeof(one_block)) == 0x0008ULL);

  // A clear continuation bit ends the field even if more bytes follow (e.g. E2E trailer bytes
  // that a non-780G model would append).
  const uint8_t trailing[] = {0x08, 0x00, 0xff, 0xff};
  check("parse stops at clear continuation bit",
        minimed_idd_flags_parse(trailing, sizeof(trailing)) == 0x0008ULL);

  check("parse of empty buffer is 0", minimed_idd_flags_parse(NULL, 0) == 0);
  const uint8_t one_byte[] = {0xef};
  check("parse of 1-byte buffer is 0 (no whole block)",
        minimed_idd_flags_parse(one_byte, 1) == 0);

  check("encode 16-bit width", minimed_idd_flags_encode(0x0008ULL, out) == 2
        && out[0] == 0x08 && out[1] == 0x00);

  // encode(parse(x)) == x for the observed vector.
  uint16_t n = minimed_idd_flags_encode(0x004f81efULL, out);
  check("encode round-trips observed vector",
        n == 4 && memcmp(out, observed, 4) == 0);

  // Continuation bits are recomputed from width: the same real bits with a stale/absent
  // continuation bit encode identically (a 16-bit and a 32-bit observation were unioned).
  n = minimed_idd_flags_encode(0x004f01efULL, out);
  check("encode recomputes continuation bits",
        n == 4 && memcmp(out, observed, 4) == 0);

  // 48-bit width: bit 33 forces three blocks; bits 15 and 31 set, block 2 terminal.
  const uint8_t want48[] = {0x01, 0x80, 0x04, 0x80, 0x02, 0x00};
  n = minimed_idd_flags_encode((1ULL << 33) | (1ULL << 18) | 1ULL, out);
  check("encode 48-bit width", n == 6 && memcmp(out, want48, 6) == 0);

  // Bit 47 is the (unused) continuation position of the last block -- structural, never a real
  // flag; it must be masked, not encoded as a flag to clear.
  n = minimed_idd_flags_encode((1ULL << 47) | 1ULL, out);
  check("structural bit 47 is masked", n == 2 && out[0] == 0x01 && out[1] == 0x00);

  check("encode of 0 is one empty block", minimed_idd_flags_encode(0, out) == 2
        && out[0] == 0x00 && out[1] == 0x00);
  printf("\n");
}

// --- Section 7: pump status (IDD Status + TAS parse, label mapping, countdowns) ---
// Ported from the bridge's iterated readStatus/statusForWatch; these tests pin the priority
// chain and the countdown stamping rules the bridge needed field iteration to get right.

static void section_status(void) {
  printf("--- pump status ---\n");
  char out[20];

  // Parse: therapy RUN, op READY, reservoir 140 IU (medfloat32 8c 00 00 00), flags 0x01
  // (reservoir attached), connectivity 0x03 (on+paired), message NO_MESSAGE.
  const uint8_t idd_normal[] = {0x55, 0x96, 0x8c, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00};
  MinimedIddStatus st;
  check("IDD status parses", minimed_status_parse_idd(idd_normal, sizeof(idd_normal), &st));
  check("IDD fields decoded", st.valid && st.therapy == 0x55 && st.operational == 0x96 &&
        st.flags == 0x01 && st.sensor_conn == 0x03 && st.sensor_msg == 0x00 &&
        st.reservoir_mu == 140000);
  check("IDD wrong length rejected", !minimed_status_parse_idd(idd_normal, 8, &st));

  // TAS: opcode 0x03FE, flags auto-mode only, shield AUTO_BASAL, readiness NO_ACTION.
  const uint8_t tas_normal[] = {0xFE, 0x03, 0x01, 0x00, 0x02, 0x00};
  MinimedTas tas;
  check("TAS parses", minimed_status_parse_tas(tas_normal, sizeof(tas_normal), &tas));
  check("TAS fields decoded", tas.valid && tas.has_auto_mode && tas.shield == 0x02 &&
        tas.readiness == 0x00 && tas.temp_target_min == 0);
  const uint8_t tas_bad_op[] = {0xFC, 0x03, 0x01, 0x00, 0x02, 0x00};
  check("TAS wrong opcode rejected", !minimed_status_parse_tas(tas_bad_op, sizeof(tas_bad_op), &tas));
  // Field-order check: flags auto+LGS+PLGM+temp-target (0x0F). tas.py consumption order is
  // auto(2B), plgm(1B), lgs(1B), then temp target (2B LE) -- so tt must be read at offset 8.
  const uint8_t tas_order[] = {0xFE, 0x03, 0x0F, 0x00, 0x02, 0x00, 0x11, 0x22, 0x3C, 0x00};
  check("TAS flag-gated field order (tt=60 at offset 8)",
        minimed_status_parse_tas(tas_order, sizeof(tas_order), &tas) && tas.temp_target_min == 60);
  // Trailing bytes tolerated (bridge behaviour; a stray E2E trailer must not kill the parse).
  const uint8_t tas_trail[] = {0xFE, 0x03, 0x01, 0x00, 0x02, 0x00, 0xAA, 0xBB, 0xCC};
  check("TAS trailing bytes tolerated", minimed_status_parse_tas(tas_trail, sizeof(tas_trail), &tas));

  // Mapping: normal -> "" (nothing shown).
  minimed_status_reset();
  minimed_status_parse_idd(idd_normal, sizeof(idd_normal), &st);
  minimed_status_parse_tas(tas_normal, sizeof(tas_normal), &tas);
  minimed_status_update(&st, &tas, 1000);
  check("normal composes to empty", minimed_status_compose(1000, out, sizeof(out)) && out[0] == '\0');
  check("normal does not tick", !minimed_status_ticking());
  check("normal BG valid", !minimed_status_bg_invalid());

  // Suspended: therapy STOP + op READY -> "SUSPENDED", count-up from entry.
  minimed_status_reset();
  MinimedIddStatus sus = st;
  sus.therapy = 0x33;
  minimed_status_update(&sus, &tas, 1000);
  minimed_status_compose(1000, out, sizeof(out));
  check("suspend at entry", strcmp(out, "SUSPENDED 0:00") == 0);
  minimed_status_update(&sus, &tas, 1000 + 300);  // still suspended 5 min later
  minimed_status_compose(1000 + 300, out, sizeof(out));
  check("suspend counts up (not restamped)", strcmp(out, "SUSPENDED 0:05") == 0);
  check("suspend ticks", minimed_status_ticking());

  // Load reservoir outranks plain suspend: therapy STOP but op mid-procedure.
  MinimedIddStatus load = sus;
  load.operational = 0x5A;  // PRIMING
  minimed_status_update(&load, &tas, 2000);
  minimed_status_compose(2000, out, sizeof(out));
  check("load reservoir label", strcmp(out, "LOAD RESERVOIR") == 0);

  // Warm-up: self-timed 2 h countdown, stamped on entry only.
  minimed_status_reset();
  MinimedIddStatus warm = st;
  warm.sensor_msg = 0x08;  // WARM_UP
  minimed_status_update(&warm, &tas, 10000);
  minimed_status_compose(10000 + 60, out, sizeof(out));
  check("warm-up countdown after 1 min", strcmp(out, "WARM-UP 1:59") == 0);
  minimed_status_update(&warm, &tas, 10000 + 600);  // re-read 10 min in: must NOT restart the clock
  minimed_status_compose(10000 + 600, out, sizeof(out));
  check("warm-up expiry not restamped", strcmp(out, "WARM-UP 1:50") == 0);
  check("warm-up means BG invalid", minimed_status_bg_invalid());
  minimed_status_update(&st, &tas, 10000 + 700);  // sensor live again
  check("warm-up exit clears BG-invalid", !minimed_status_bg_invalid());
  minimed_status_update(&warm, &tas, 20000);  // re-enter: fresh 2 h
  minimed_status_compose(20000, out, sizeof(out));
  check("warm-up re-entry restamps", strcmp(out, "WARM-UP 2:00") == 0);

  // GST signal lost (connectivity bit 2) invalidates BG even with no sensor message.
  minimed_status_reset();
  MinimedIddStatus lost = st;
  lost.sensor_conn = 0x07;  // on + paired + signal lost
  minimed_status_update(&lost, &tas, 3000);
  check("GST signal lost means BG invalid", minimed_status_bg_invalid());

  // SG off-scale: no band (the BG shows LO/HI instead, so the band would only cover the graph);
  // the accessors report which side; BG stays valid so LO/HI isn't blanked to "---".
  MinimedIddStatus low = st;
  low.sensor_msg = 0x09;  // SG_BELOW_LOWER_LIMIT
  minimed_status_update(&low, &tas, 3100);
  minimed_status_compose(3100, out, sizeof(out));
  check("SG below composes to empty", out[0] == '\0');
  check("SG below reported", minimed_status_sg_below() && !minimed_status_sg_above());
  check("SG below keeps BG valid", !minimed_status_bg_invalid());
  MinimedIddStatus high = st;
  high.sensor_msg = 0x0A;  // SG_ABOVE_UPPER_LIMIT
  minimed_status_update(&high, &tas, 3200);
  minimed_status_compose(3200, out, sizeof(out));
  check("SG above composes to empty", out[0] == '\0');
  check("SG above reported", minimed_status_sg_above() && !minimed_status_sg_below());
  minimed_status_update(&st, &tas, 3300);
  check("off-scale clears on return to normal",
        !minimed_status_sg_below() && !minimed_status_sg_above());

  // Temp target: restamped from the pump's live minutes each read; counts down.
  minimed_status_reset();
  MinimedTas tt = tas;
  tt.temp_target_min = 60;
  minimed_status_update(&st, &tt, 5000);
  minimed_status_compose(5000 + 120, out, sizeof(out));
  check("temp target countdown", strcmp(out, "TEMP TARGET 0:58") == 0);
  check("temp target ticks", minimed_status_ticking());

  // SmartGuard off / safe basal from the shield; BG REQUIRED outranks them.
  minimed_status_reset();
  MinimedTas open = tas;
  open.shield = 0x01;  // OPEN_LOOP
  minimed_status_update(&st, &open, 6000);
  minimed_status_compose(6000, out, sizeof(out));
  check("open loop -> SMARTGUARD OFF", strcmp(out, "SMARTGUARD OFF") == 0);
  open.readiness = 1;  // BG_REQUIRED
  minimed_status_update(&st, &open, 6100);
  minimed_status_compose(6100, out, sizeof(out));
  check("BG required outranks loop state", strcmp(out, "BG REQUIRED") == 0);

  // Both reads failed: previous state survives untouched.
  MinimedIddStatus bad_st = {.valid = false};
  MinimedTas bad_tas = {.valid = false};
  minimed_status_update(&bad_st, &bad_tas, 6200);
  minimed_status_compose(6200, out, sizeof(out));
  check("double read failure keeps last label", strcmp(out, "BG REQUIRED") == 0);

  // TAS-only (IDD read failed): loop-state clauses still fire.
  minimed_status_reset();
  minimed_status_update(&bad_st, &open, 6300);
  minimed_status_compose(6300, out, sizeof(out));
  check("TAS-only read still maps", strcmp(out, "BG REQUIRED") == 0);

  // Nothing ever seen: compose refuses.
  minimed_status_reset();
  check("compose refuses before first data", !minimed_status_compose(0, out, sizeof(out)));
  printf("\n");
}

// --- Section 8: pump annunciations (history record parse + name table) ---
// Record layout and the consolidated-event fields follow PythonPumpConnector history/data.py
// (AnnunciationData); vectors are synthetic per that format. The one field-confirmed code is
// 0x054 = insert battery (bridge capture 2026-07-20, status=0x0f while raised).

static void section_annunciation(void) {
  printf("--- annunciations ---\n");
  MinimedAnnunciation a;

  // Consolidated LOW_SG_SUSPEND_ALERT (raw type 0xf323): header(8) + flags/id/type/status/
  // timestamp(10) + aux sg+time(4). Flags 0x0f = auxinfo1-4 present, not silenced.
  const uint8_t low_sg[] = {0x10, 0xf0, 0x40, 0xe2, 0x01, 0x00, 0x58, 0x02,
                            0x0f, 0x42, 0x00, 0x23, 0xf3, 0x33, 0x78, 0x56,
                            0x34, 0x12, 0x2c, 0x01, 0x05, 0x02};
  check("consolidated parses",
        minimed_annunciation_parse_record(low_sg, sizeof(low_sg), &a) == MinimedAnnuncRecordYes);
  check("consolidated fields", a.seq == 123456 && a.type == 0x323 && a.id == 0x42 &&
        a.status == 0x33 && !a.silenced);

  // Silenced INSERT_BATTERY_ALERT (flags bit 6), no aux beyond the timestamp, len exactly 18.
  const uint8_t silenced[] = {0x10, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x43, 0x07, 0x00, 0x54, 0xf0, 0x0f, 0x00, 0x00, 0x00, 0x00};
  check("silenced flag decoded",
        minimed_annunciation_parse_record(silenced, sizeof(silenced), &a) ==
            MinimedAnnuncRecordYes && a.type == 0x054 && a.status == 0x0f && a.silenced);

  // Minimum-length consolidated: complete through the status byte (14 bytes), timestamp absent.
  const uint8_t min_len[] = {0x10, 0xf0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x03, 0x01, 0x00, 0x54, 0xf0, 0x33};
  check("14-byte consolidated parses",
        minimed_annunciation_parse_record(min_len, sizeof(min_len), &a) ==
            MinimedAnnuncRecordYes && a.seq == 5 && a.type == 0x054);
  check("13 bytes is bad",
        minimed_annunciation_parse_record(min_len, 13, &a) == MinimedAnnuncRecordBad);
  check("bad still yields seq", a.seq == 5);

  // Another event type (SG Measurement 0xf00c): skipped, but its seq still advances the cursor.
  const uint8_t sg_meas[] = {0x0c, 0xf0, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x05, 0x00, 0x7a, 0x00, 0xff, 0x03, 0x01, 0x00};
  check("other event type is Other",
        minimed_annunciation_parse_record(sg_meas, sizeof(sg_meas), &a) ==
            MinimedAnnuncRecordOther && a.seq == 0x99);

  // Annunciation Cleared (0xf00f) is deliberately Other: raise-only notifications.
  const uint8_t cleared[] = {0x0f, 0xf0, 0x9a, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x54, 0xf0, 0x07, 0x00};
  check("cleared event is Other",
        minimed_annunciation_parse_record(cleared, sizeof(cleared), &a) ==
            MinimedAnnuncRecordOther && a.seq == 0x9a);

  // Consolidated whose type field lacks the 0xf000 nibble: misaligned/garbled, rejected.
  const uint8_t bad_nibble[] = {0x10, 0xf0, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x03, 0x01, 0x00, 0x54, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00};
  check("type without 0xf000 nibble is bad",
        minimed_annunciation_parse_record(bad_nibble, sizeof(bad_nibble), &a) ==
            MinimedAnnuncRecordBad);

  check("shorter than a header is bad",
        minimed_annunciation_parse_record(low_sg, 7, &a) == MinimedAnnuncRecordBad);

  // Name table: the field-confirmed code, the one alert Morten cares most about, and a resume.
  check("0x054 named", minimed_annunciation_name(0x054) != NULL &&
        strcmp(minimed_annunciation_name(0x054), "Insert battery") == 0);
  check("0x323 named", minimed_annunciation_name(0x323) != NULL &&
        strcmp(minimed_annunciation_name(0x323), "Low SG suspend") == 0);
  // Pump wording, confirmed on HW 2026-08-19 (two real alerts): the pump calls 0x325
  // "Alert before low", not the table's earlier "Low predicted".
  check("0x325 uses pump wording", minimed_annunciation_name(0x325) != NULL &&
        strcmp(minimed_annunciation_name(0x325), "Alert before low") == 0);
  check("0x33b named", minimed_annunciation_name(0x33b) != NULL &&
        strcmp(minimed_annunciation_name(0x33b), "Severe low SG") == 0);
  check("unknown code has no name", minimed_annunciation_name(0x999) == NULL);
  printf("\n");
}

int main(void) {
  printf("=== SAKE C port host verification ===\n\n");
  section_primitives();
  section_captured_trace();
  section_seqcrypt();
  section_iob();
  section_graph();
  section_idd_flags();
  section_status();
  section_annunciation();
  printf("SUMMARY: %d passed, %d failed -> %s\n", g_pass, g_fail,
         g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
  return g_fail == 0 ? 0 : 1;
}
