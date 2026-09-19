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
#include "minimed_glucose_announce.h"
#include "pebble_glucose_protocol.h"
#include "minimed_graph.h"
#include "minimed_history.h"
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
// Live readings append; the pump's event log fills in the past on connect. The awkward cases are all about keeping the array strictly ascending (the offset-from-oldest wire
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

  check("empty graph serializes to nothing",
        minimed_graph_serialize(&g, MINIMED_GRAPH_MAX_HOURS * 60 * 60, g_blob) == 0);

  minimed_graph_add(&g, T0, 100);
  minimed_graph_add(&g, T0 + MIN(5), 110);
  minimed_graph_add(&g, T0 + MIN(10), 121);
  g_blob_len = minimed_graph_serialize(&g, MINIMED_GRAPH_MAX_HOURS * 60 * 60, g_blob);
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
  minimed_graph_add(&w, T0 + MINIMED_GRAPH_RETENTION_SECS, 120);
  check("point exactly one window old is dropped", w.count == 1 && w.bg[0] == 60);

  // Overflow: feed more points than the buffer holds. Spacing must be >= 1 min or the /60 in the
  // wire encoding collapses every offset to 0 and the ascending check below proves nothing --
  // 4 min keeps the points inside the retention window, so eviction is by capacity.
  MinimedGraph f = {0};
  const int n_fill = MINIMED_GRAPH_MAX_POINTS + 10;
  for (int i = 0; i < n_fill; i++) {
    minimed_graph_add(&f, T0 + MIN(4 * i), (int32_t)(100 + i));
  }
  check("buffer caps at MAX_POINTS", f.count == MINIMED_GRAPH_MAX_POINTS);
  check("oldest points are the ones evicted", f.ts[0] == T0 + MIN(4 * 10));
  check("newest point is retained", f.ts[f.count - 1] == T0 + MIN(4 * (n_fill - 1)));

  // A requested one-hour window includes the fixed 30-minute margin, but not older points.
  MinimedGraph filtered = {0};
  minimed_graph_add(&filtered, T0, 100);
  minimed_graph_add(&filtered, T0 + MIN(30), 110);
  minimed_graph_add(&filtered, T0 + MIN(60), 120);
  minimed_graph_add(&filtered, T0 + MIN(90), 130);
  minimed_graph_add(&filtered, T0 + MIN(120), 140);
  g_blob_len = minimed_graph_serialize(&filtered, 60 * 60, g_blob);
  check("zero graph window serializes to nothing",
        minimed_graph_serialize(&filtered, 0, g_blob) == 0);
  check("graph window includes margin", g_blob_len == 6 + 3 * 4 && blob_ref() == T0 + MIN(30));
  check("graph window keeps newest points", blob_count() == 4 && blob_bg(3) == 70);

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
  g_blob_len = minimed_graph_serialize(&f, MINIMED_GRAPH_MAX_HOURS * 60 * 60, g_blob);
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

// --- Section 5b: backfill of past readings from the pump's event log -------
static void section_backfill(void) {
  MinimedGraph g = {0};
  minimed_graph_add(&g, T0 + MIN(60), 100);  // live reading
  minimed_graph_insert_past(&g, T0 + MIN(50), 90);
  minimed_graph_insert_past(&g, T0 + MIN(30), 80);
  minimed_graph_insert_past(&g, T0 + MIN(40), 84);
  check("past points land in order",
        g.count == 4 && g.ts[0] == T0 + MIN(30) && g.ts[1] == T0 + MIN(40) &&
            g.ts[2] == T0 + MIN(50) && g.ts[3] == T0 + MIN(60));
  check("past point values follow their timestamps",
        g.bg[0] == 40 && g.bg[1] == 42 && g.bg[2] == 45 && g.bg[3] == 50);

  // The live reading was stamped on arrival, so its backfilled twin is a minute or so off.
  minimed_graph_insert_past(&g, T0 + MIN(60) - 70, 100);
  minimed_graph_insert_past(&g, T0 + MIN(50) + 60, 91);
  check("near-duplicates are dropped", g.count == 4);
  minimed_graph_insert_past(&g, T0 + MIN(20), -1);
  check("negative mg/dL is ignored", g.count == 4);

  MinimedGraph e = {0};
  minimed_graph_insert_past(&e, T0, 100);
  check("insert into an empty graph works", e.count == 1 && e.ts[0] == T0);

  MinimedGraph w = {0};
  minimed_graph_add(&w, T0 + MINIMED_GRAPH_RETENTION_SECS + MIN(10), 100);
  minimed_graph_insert_past(&w, T0, 100);
  check("a point older than the window is dropped", w.count == 1);

  // Full buffer: room is made from the oldest end, but never for a point older than everything.
  MinimedGraph f = {0};
  for (int i = 0; i < MINIMED_GRAPH_MAX_POINTS; i++) {
    minimed_graph_add(&f, T0 + 299u * i, 100);
  }
  const uint32_t oldest = f.ts[0];
  minimed_graph_insert_past(&f, oldest - MIN(3), 100);
  check("full graph ignores a point older than all", f.count == MINIMED_GRAPH_MAX_POINTS &&
                                                       f.ts[0] == oldest);
  minimed_graph_insert_past(&f, f.ts[5] + 150, 100);
  bool ascending = true;
  for (int i = 1; i < f.count; i++) ascending &= f.ts[i] > f.ts[i - 1];
  check("full graph inserts in the middle and stays ascending",
        f.count == MINIMED_GRAPH_MAX_POINTS && ascending && f.ts[0] > oldest);

  // Real records from a pump's event log (database/history.db): the 03:38 reference time, the
  // two samples logged together at 03:47:46 (offsets 4 and 9, so 5 min apart), and the next one.
  static const uint8_t ref0338[] = {0x0e, 0xf0, 0x32, 0x4d, 0x0b, 0x00, 0x10, 0x0e,
                                    0x3c, 0xea, 0x07, 0x06, 0x1a, 0x03, 0x26, 0x00};
  static const uint8_t sg_a[] = {0x0c, 0xf0, 0x3b, 0x4d, 0x0b, 0x00, 0x4a, 0x02,
                                 0x04, 0x00, 0x36, 0x00, 0xf1, 0x04, 0xc7, 0xff};
  static const uint8_t sg_b[] = {0x0c, 0xf0, 0x3d, 0x4d, 0x0b, 0x00, 0x4a, 0x02,
                                 0x09, 0x00, 0x47, 0x00, 0xe6, 0x05, 0xc7, 0xff};
  static const uint8_t sg_c[] = {0x0c, 0xf0, 0x3f, 0x4d, 0x0b, 0x00, 0x6f, 0x03,
                                 0x0e, 0x00, 0x4c, 0x00, 0x89, 0x06, 0xc8, 0xff};
  static const uint8_t sg_below[] = {0x0c, 0xf0, 0x5b, 0x4d, 0x0b, 0x00, 0x16, 0x01,
                                     0x04, 0x00, 0x0d, 0x03, 0x72, 0x04, 0xcc, 0xff};
  MinimedHistRef ref;
  MinimedHistSg a, b, c2, bl;
  check("real reference time parses",
        minimed_history_parse_ref_time(ref0338, sizeof(ref0338), &ref) && ref.seq == 740658);
  check("real SG records parse", minimed_history_parse_sg(sg_a, sizeof(sg_a), &a) &&
                                     minimed_history_parse_sg(sg_b, sizeof(sg_b), &b) &&
                                     minimed_history_parse_sg(sg_c, sizeof(sg_c), &c2) &&
                                     minimed_history_parse_sg(sg_below, sizeof(sg_below), &bl));
  check("real SG values", a.sg == 54 && b.sg == 71 && c2.sg == 76 && a.offset_min == 4 &&
                              b.offset_min == 9 && c2.offset_min == 14);
  check("samples logged together are 5 min apart by their offsets",
        minimed_history_sg_secs(&ref, &b) - minimed_history_sg_secs(&ref, &a) == 300 &&
            minimed_history_sg_secs(&ref, &c2) - minimed_history_sg_secs(&ref, &b) == 300);
  // 03:38:00 + 4 min = 03:42:00, against the same day's midnight.
  MinimedHistRef midnight = ref;
  static const uint8_t ref0000[] = {0x0e, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x3c, 0xea, 0x07, 0x06, 0x1a, 0x00, 0x00, 0x00};
  check("reference time decodes the date",
        minimed_history_parse_ref_time(ref0000, sizeof(ref0000), &midnight) &&
            ref.secs - midnight.secs == 3 * 3600 + 38 * 60);
  static const uint8_t ref_prev_day[] = {0x0e, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x3c, 0xea, 0x07, 0x06, 0x19, 0x17, 0x26, 0x00};
  MinimedHistRef prev;
  check("reference time spans midnight",
        minimed_history_parse_ref_time(ref_prev_day, sizeof(ref_prev_day), &prev) &&
            midnight.secs - prev.secs == 22 * 60);
  static const uint8_t ref_bad[] = {0x0e, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x3c, 0xea, 0x07, 0x0d, 0x1a, 0x03, 0x26, 0x00};
  check("reference time with month 13 is rejected",
        !minimed_history_parse_ref_time(ref_bad, sizeof(ref_bad), &prev));
  check("an SG record is not a reference time",
        !minimed_history_parse_ref_time(sg_a, sizeof(sg_a), &prev));
  check("real below-range sample maps to the floor",
        minimed_history_sg_to_mgdl(bl.sg, 50, 400) == 50);

  // History SG records.
  const uint8_t rec[] = {0x0c, 0xf0, 0x39, 0x05, 0x00, 0x00, 0x2a, 0x00,  // type, seq=1337, rel
                         0x31, 0x24, 0x8d, 0x00, 0x11, 0x02, 0x05, 0x00};   // off=9265 sg=141
  MinimedHistSg sg;
  check("SG record parses", minimed_history_parse_sg(rec, sizeof(rec), &sg) && sg.seq == 1337 &&
                                sg.offset_min == 9265 && sg.sg == 141);
  check("SG truncated after the value still parses",
        minimed_history_parse_sg(rec, 12, &sg) && sg.sg == 141);
  check("SG too short is rejected", !minimed_history_parse_sg(rec, 11, &sg));
  uint8_t other[sizeof(rec)];
  for (unsigned i = 0; i < sizeof(rec); i++) other[i] = rec[i];
  other[1] = 0xf1;
  check("another event type is not an SG record", !minimed_history_parse_sg(other, sizeof(other), &sg));
  check("normal SG maps to itself", minimed_history_sg_to_mgdl(141, 50, 400) == 141);
  check("below-range code maps to the floor",
        minimed_history_sg_to_mgdl(MINIMED_HIST_SG_BELOW, 50, 400) == 50);
  check("above-range code maps to the ceiling",
        minimed_history_sg_to_mgdl(MINIMED_HIST_SG_ABOVE, 50, 400) == 400);
  check("starting/updating/zero yield no sample",
        minimed_history_sg_to_mgdl(MINIMED_HIST_SG_STARTING, 50, 400) == -1 &&
            minimed_history_sg_to_mgdl(MINIMED_HIST_SG_UPDATING, 50, 400) == -1 &&
            minimed_history_sg_to_mgdl(0, 50, 400) == -1);

  // Meal records (0xf005): food amount as a MedFloat16.
  const uint8_t meal[] = {0x05, 0xf0, 0x10, 0x00, 0x00, 0x00, 0x05, 0x00, 0x2d, 0x00};  // 45 g
  MinimedHistMeal m;
  check("meal record parses", minimed_history_parse_meal(meal, sizeof(meal), &m) && m.seq == 16 &&
                                  m.grams == 45);
  const uint8_t meal_dec[] = {0x05, 0xf0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0xf0};  // 12.5
  check("fractional grams round half up",
        minimed_history_parse_meal(meal_dec, sizeof(meal_dec), &m) && m.grams == 13);
  const uint8_t meal_nan[] = {0x05, 0xf0, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x07};
  check("NaN food amount is rejected", !minimed_history_parse_meal(meal_nan, sizeof(meal_nan), &m));
  const uint8_t meal_neg[] = {0x05, 0xf0, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfb, 0x0f};
  check("negative food amount is rejected", !minimed_history_parse_meal(meal_neg, sizeof(meal_neg), &m));
  check("meal record too short is rejected", !minimed_history_parse_meal(meal, 9, &m));
  check("an SG record is not a meal", !minimed_history_parse_meal(rec, sizeof(rec), &m));
  const uint8_t meal_zero[] = {0x05, 0xf0, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  check("zero grams parses as zero", minimed_history_parse_meal(meal_zero, sizeof(meal_zero), &m) &&
                                        m.grams == 0);
  // Real Meal records from a pump's event log: 59 g, 65 g, and a zero-carb one.
  static const uint8_t real59[] = {0x05, 0xf0, 0xb1, 0x4d, 0x0b, 0x00, 0x30, 0x0a, 0x3b, 0x00};
  static const uint8_t real65[] = {0x05, 0xf0, 0xc8, 0x4a, 0x0b, 0x00, 0x36, 0x0d, 0x41, 0x00};
  static const uint8_t real0[] = {0x05, 0xf0, 0xbc, 0x4c, 0x0b, 0x00, 0xf4, 0x05, 0x00, 0x00};
  check("real meal records parse",
        minimed_history_parse_meal(real59, sizeof(real59), &m) && m.grams == 59 &&
            minimed_history_parse_meal(real65, sizeof(real65), &m) && m.grams == 65 &&
            minimed_history_parse_meal(real0, sizeof(real0), &m) && m.grams == 0);
  check("history-event flag is bit 7", MINIMED_IDD_FLAG_HISTORY_EVENT == 0x80);
}

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
  check("normal composes to empty", minimed_status_compose(out, sizeof(out)) && out[0] == '\0');
  MinimedStatusTimers timers = minimed_status_get_timers();
  check("normal does not tick", timers.start == 0 && timers.end == 0);
  check("normal BG valid", !minimed_status_bg_invalid());

  // Sensor recovery latch: fires once on invalid -> valid, and is consumed by the read.
  // The pump can resume showing glucose without setting its "new CGM" push bit, so this is what
  // makes the watch fetch a reading instead of waiting out the 6-minute fallback.
  minimed_status_reset();
  minimed_status_parse_tas(tas_normal, sizeof(tas_normal), &tas);
  MinimedIddStatus gone = st;
  gone.sensor_conn = 0x07;  // on + paired + signal lost
  minimed_status_update(&gone, &tas, 1000);
  check("signal lost -> BG invalid", minimed_status_bg_invalid());
  check("no recovery latch while still invalid", !minimed_status_take_bg_became_valid());
  minimed_status_update(&st, &tas, 1060);  // sensor back
  check("recovery latches", minimed_status_bg_invalid() == false);
  check("recovery latch reads true once", minimed_status_take_bg_became_valid());
  check("recovery latch is consumed", !minimed_status_take_bg_became_valid());
  minimed_status_update(&st, &tas, 1120);  // still valid: no new transition
  check("no latch without a transition", !minimed_status_take_bg_became_valid());

  // Suspended: therapy STOP + op READY -> "SUSPENDED", count-up from entry.
  minimed_status_reset();
  MinimedIddStatus sus = st;
  sus.therapy = 0x33;
  minimed_status_update(&sus, &tas, 1000);
  minimed_status_compose(out, sizeof(out));
  check("suspend at entry", strcmp(out, "SUSPENDED") == 0);
  minimed_status_update(&sus, &tas, 1000 + 300);  // still suspended 5 min later
  minimed_status_compose(out, sizeof(out));
  check("suspend label remains stable", strcmp(out, "SUSPENDED") == 0);
  timers = minimed_status_get_timers();
  check("suspend timer starts at entry", timers.start == 1000 && timers.end == 0);

  // Load reservoir outranks plain suspend: therapy STOP but op mid-procedure.
  MinimedIddStatus load = sus;
  load.operational = 0x5A;  // PRIMING
  minimed_status_update(&load, &tas, 2000);
  minimed_status_compose(out, sizeof(out));
  check("load reservoir label", strcmp(out, "LOAD RESERVOIR") == 0);

  // Warm-up: self-timed 2 h countdown, stamped on entry only.
  minimed_status_reset();
  MinimedIddStatus warm = st;
  warm.sensor_msg = 0x08;  // WARM_UP
  minimed_status_update(&warm, &tas, 10000);
  minimed_status_compose(out, sizeof(out));
  check("warm-up label", strcmp(out, "WARM-UP") == 0);
  minimed_status_update(&warm, &tas, 10000 + 600);  // re-read 10 min in: must NOT restart the clock
  minimed_status_compose(out, sizeof(out));
  check("warm-up label remains stable", strcmp(out, "WARM-UP") == 0);
  check("warm-up means BG invalid", minimed_status_bg_invalid());
  minimed_status_update(&st, &tas, 10000 + 700);  // sensor live again
  check("warm-up exit clears BG-invalid", !minimed_status_bg_invalid());
  minimed_status_update(&warm, &tas, 20000);  // re-enter: fresh 2 h
  minimed_status_compose(out, sizeof(out));
  check("warm-up re-entry", strcmp(out, "WARM-UP") == 0);

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
  minimed_status_compose(out, sizeof(out));
  check("SG below composes to empty", out[0] == '\0');
  check("SG below reported", minimed_status_sg_below() && !minimed_status_sg_above());
  check("SG below keeps BG valid", !minimed_status_bg_invalid());
  MinimedIddStatus high = st;
  high.sensor_msg = 0x0A;  // SG_ABOVE_UPPER_LIMIT
  minimed_status_update(&high, &tas, 3200);
  minimed_status_compose(out, sizeof(out));
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
  minimed_status_compose(out, sizeof(out));
  check("temp target label", strcmp(out, "TEMP TARGET") == 0);
  timers = minimed_status_get_timers();
  check("temp target timer has an end", timers.start == 0 && timers.end == 5000 + 60 * 60);

  // SmartGuard off / safe basal from the shield; BG REQUIRED outranks them.
  minimed_status_reset();
  MinimedTas open = tas;
  open.shield = 0x01;  // OPEN_LOOP
  minimed_status_update(&st, &open, 6000);
  minimed_status_compose(out, sizeof(out));
  check("open loop -> SMARTGUARD OFF", strcmp(out, "SMARTGUARD OFF") == 0);
  open.readiness = 1;  // BG_REQUIRED
  minimed_status_update(&st, &open, 6100);
  minimed_status_compose(out, sizeof(out));
  check("BG required outranks loop state", strcmp(out, "BG REQUIRED") == 0);

  // Both reads failed: previous state survives untouched.
  MinimedIddStatus bad_st = {.valid = false};
  MinimedTas bad_tas = {.valid = false};
  minimed_status_update(&bad_st, &bad_tas, 6200);
  minimed_status_compose(out, sizeof(out));
  check("double read failure keeps last label", strcmp(out, "BG REQUIRED") == 0);

  // TAS-only (IDD read failed): loop-state clauses still fire.
  minimed_status_reset();
  minimed_status_update(&bad_st, &open, 6300);
  minimed_status_compose(out, sizeof(out));
  check("TAS-only read still maps", strcmp(out, "BG REQUIRED") == 0);

  // Nothing ever seen: compose refuses.
  minimed_status_reset();
  check("compose refuses before first data", !minimed_status_compose(out, sizeof(out)));
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
  // Confirmed on HW 2026-08-30: 0x31a = sensor expired, 0x33f = SmartGuard calibration timeout,
  // 0x071 = reservoir empty.
  check("0x31a named", minimed_annunciation_name(0x31a) != NULL &&
        strcmp(minimed_annunciation_name(0x31a), "Sensor expired") == 0);
  check("0x33f named", minimed_annunciation_name(0x33f) != NULL &&
        strcmp(minimed_annunciation_name(0x33f), "SmartGuard calibration timeout") == 0);
  check("0x071 named", minimed_annunciation_name(0x071) != NULL &&
        strcmp(minimed_annunciation_name(0x071), "Reservoir empty") == 0);
  // Reported (not our own HW confirmation): times out a started bolus without delivering it.
  check("0x064 named", minimed_annunciation_name(0x064) != NULL &&
        strcmp(minimed_annunciation_name(0x064), "Bolus not delivered") == 0);
  check("unknown code has no name", minimed_annunciation_name(0x999) == NULL);

  // BG-suffix predicate: only the predicted/impending-low family says yes.
  check("0x325 (alert before low) shows BG", minimed_annunciation_shows_bg(0x325));
  check("0x32b (suspend before low) shows BG", minimed_annunciation_shows_bg(0x32b));
  check("0x323 (already-low suspend) does not show BG", !minimed_annunciation_shows_bg(0x323));
  check("0x068 (low battery, unrelated to BG) does not show BG",
        !minimed_annunciation_shows_bg(0x068));
  printf("\n");
}

// --- Section 9: glucose-protocol capability announcement ------------------
// Doubles as the "is this watchface one of ours?" test: we must claim a watchface's UUID before
// we can receive anything from it, so the claim is provisional and this parse confirms it.

// Build a serialized dictionary the way dict.c does: [u8 count] then per tuple
// [u32 key][u8 type][u16 len][value].
static uint8_t g_dict[128];
static uint16_t g_dict_len;

static void dict_begin(uint8_t count) {
  g_dict[0] = count;
  g_dict_len = 1;
}

static void dict_put(uint32_t key, uint8_t type, uint16_t len, const uint8_t *val) {
  uint8_t *p = g_dict + g_dict_len;
  p[0] = (uint8_t)key; p[1] = (uint8_t)(key >> 8);
  p[2] = (uint8_t)(key >> 16); p[3] = (uint8_t)(key >> 24);
  p[4] = type;
  p[5] = (uint8_t)len; p[6] = (uint8_t)(len >> 8);
  memcpy(p + 7, val, len);
  g_dict_len += 7 + len;
}

static void dict_put_uint(uint32_t key, uint32_t value, uint16_t width) {
  uint8_t v[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                  (uint8_t)(value >> 24)};
  dict_put(key, 2 /* TUPLE_UINT */, width, v);
}

// What pebble-glucose-watchface actually sends (main.c: uint8 version, uint32 caps, uint8 hours).
static void dict_real_announce(void) {
  dict_begin(3);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG | CAP_IOB | CAP_STATUS, 4);
  dict_put_uint(KEY_GRAPH_HOURS, 24, 1);
}

static void section_announce(void) {
  printf("  Section 9: capability announcement parse\n");
  MinimedGlucoseAnnounce a;

  dict_real_announce();
  check("real watchface announcement accepted",
        minimed_glucose_parse_announce(g_dict, g_dict_len, &a));
  check("caps decoded", a.caps == (CAP_BG | CAP_IOB | CAP_STATUS));
  check("graph hours decoded", a.graph_hours == 24);
  check("version decoded", a.version == PROTOCOL_VERSION);

  // GRAPH_HOURS is optional; its absence means no graph rather than a parse failure.
  dict_begin(2);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  check("announcement without GRAPH_HOURS accepted",
        minimed_glucose_parse_announce(g_dict, g_dict_len, &a) && a.graph_hours == 0);

  // Narrower encodings of the same number are the same number.
  dict_begin(2);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 1);
  check("uint8 capabilities accepted",
        minimed_glucose_parse_announce(g_dict, g_dict_len, &a) && a.caps == CAP_BG);

  // Unknown keys must not break us, or the protocol can never grow.
  dict_begin(3);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  dict_put_uint(7 /* reserved */, 1234, 4);
  check("unknown key ignored", minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  // --- Rejections: these are foreign watchfaces, not ours.
  dict_begin(1);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  check("version alone rejected", !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  dict_begin(1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  check("capabilities alone rejected", !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  dict_begin(2);
  dict_put_uint(KEY_PROTOCOL_VERSION, 2, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  check("wrong protocol version rejected",
        !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  dict_begin(2);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, 0, 4);
  check("zero capabilities rejected", !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  dict_begin(2);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, 0x100, 4);  // bit 8 is not a defined capability
  check("undefined capability bit rejected",
        !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  // A plausible foreign watchface: low raw keys, but strings rather than our value shapes.
  dict_begin(2);
  dict_put(KEY_PROTOCOL_VERSION, 1 /* TUPLE_CSTRING */, 4, (const uint8_t *)"abc");
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  check("cstring in key 0 rejected", !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  // Truncation must not read past the buffer.
  dict_real_announce();
  check("truncated dictionary rejected",
        !minimed_glucose_parse_announce(g_dict, (uint16_t)(g_dict_len - 3), &a));
  check("empty buffer rejected", !minimed_glucose_parse_announce(g_dict, 0, &a));

  // A count that overstates the tuples present.
  dict_begin(5);
  dict_put_uint(KEY_PROTOCOL_VERSION, PROTOCOL_VERSION, 1);
  dict_put_uint(KEY_CAPABILITIES, CAP_BG, 4);
  check("overstated tuple count rejected",
        !minimed_glucose_parse_announce(g_dict, g_dict_len, &a));

  printf("\n");
}

int main(void) {
  printf("=== SAKE C port host verification ===\n\n");
  section_primitives();
  section_captured_trace();
  section_seqcrypt();
  section_iob();
  section_graph();
  section_backfill();
  section_idd_flags();
  section_status();
  section_annunciation();
  section_announce();
  printf("SUMMARY: %d passed, %d failed -> %s\n", g_pass, g_fail,
         g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
  return g_fail == 0 ? 0 : 1;
}
