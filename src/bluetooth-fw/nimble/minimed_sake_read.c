/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_read.h"

#include <stdio.h>
#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include "drivers/rtc.h"
#include "minimed_idd_flags.h"
#include "minimed_iob.h"
#include "minimed_sake_sender.h"
#include "minimed_sake_service.h"
#include "popups/minimed_sake_spike_ui.h"
#include <system/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Standard SIG 16-bit UUIDs for the pump's CGM service (Documentation/cgm-service.md; the bridge's
// MedtronicProtocol.kt). The pump exposes these as a GATT server over the post-handshake link.
#define CGM_SERVICE_UUID 0x181F
#define CGM_MEASUREMENT_UUID 0x2AA7  // notify, SAKE-encrypted records
#define CGM_FEATURE_UUID 0x2AA8      // read, plaintext (E2E-CRC flag)
#define RACP_UUID 0x2A52             // write/indicate, plaintext control point

// RACP "Report Stored Records: Last Record" and its success response (Bluetooth SIG RACP).
static const uint8_t RACP_REPORT_LAST_RECORD[] = {0x01, 0x06};
static const uint8_t RACP_REPORT_SUCCESS[] = {0x06, 0x00, 0x01, 0x01};

// Medtronic Insulin Delivery service (vendor 128-bit base 0000XXXX-0000-1000-0000-009132591325):
// IDD service 0x100, SRCP (Status Reader Control Point) char 0x105 (write + indicate). Byte order
// is little-endian, same convention as the SAKE-port UUID in minimed_sake_service.c (last two data
// bytes = the 16-bit short code low/high: 00 01 for 0x0100, 05 01 for 0x0105).
static const ble_uuid128_t s_idd_svc_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00);
static const ble_uuid128_t s_idd_srcp_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x05, 0x01, 0x00, 0x00);
// IDD Status Changed 0x101 (read + indicate): the pump's "something changed" push, the event
// source that replaced the 60 s poll (v40). Each bit LATCHES until an SRCP Reset Status
// (0x030C + the bits to clear), so every received indication queues a reset write-back.
static const ble_uuid128_t s_idd_status_changed_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00);

// SRCP "Get Insulin On Board" request: little-endian opcode 0x03F3. NOT E2E-CRC-wrapped -- the
// 780G leaves E2E protection off for the IDD service (Documentation/idd-service.md), matching the
// bridge's srcpGet which does not append a CRC. SAKE-encrypted before it goes on the wire.
static const uint8_t SRCP_GET_IOB[] = {0xF3, 0x03};

// SRCP "Reset Status": little-endian opcode 0x030C + the flag field to clear, encoded exactly
// like 0x101's (minimed_idd_flags_encode). Clears indication latches only -- the pump keeps each
// 0x101 bit latched until reset, so this is what makes a second indication ever arrive.
static const uint8_t SRCP_RESET_STATUS[] = {0x0C, 0x03};

// Re-poll the latest record on this cadence. The sensor updates ~every 5 min; polling faster just
// re-shows the current value and keeps the on-watch reading fresh within one interval.
#define POLL_INTERVAL_SECS 60

// Push mode: once a 0x101 indication has actually arrived (not merely been subscribed to), the
// poll callout becomes a dead-man fallback at the bridge's tuned rate. CGM should push every
// ~5 min, so 6 min of silence means push is late or dead -- do one full read and re-arm. A
// silently dead push thus degrades to a 6-minute poll, the bridge's soaked trade-off.
#define FALLBACK_AFTER_SECS (6 * 60)
static bool s_push_mode;  // false until the first indication of this connection proves push

static struct ble_npl_callout s_read_co;
static struct ble_npl_callout s_poll_co;

// Exchange serialiser (spec: docs/superpowers/specs/2026-07-27-pump-push-design.md). The pump
// exchanges (CGM poll, SRCP IOB read, SRCP Reset Status) each span a write plus terminating
// indication(s), share the two reassembly buffers, and can now be triggered asynchronously by
// 0x101 pushes -- so exactly one is in flight at a time. s_op holds the in-flight op's PEND_ bit
// (0 = idle) and doubles as the SRCP-response disambiguator: the same char carries both the IOB
// response (complete at >= 7 bytes) and the short Reset Status response.
#define PEND_CGM 0x01
#define PEND_IOB 0x02
#define PEND_RESET 0x04
static uint8_t s_pending;
static uint8_t s_op;
static uint64_t s_reset_flags;  // union of received 0x101 flags awaiting a Reset Status write
static struct ble_npl_callout s_dispatch_co;    // issue the next pending exchange
static struct ble_npl_callout s_op_timeout_co;  // unwedge a lost terminating indication

// Writes are dispatched off a callout, never from a notify/indication handler: NimBLE sends an
// indication's confirmation only after the handler returns, so a synchronous write would go on
// air ahead of the confirmation the pump awaits. 200 ms is the v30-tuned CGM->IOB gap, kept.
#define DISPATCH_DELAY_MS 200
// Observed poll->notification latency is 0-3 s; NimBLE's own 30 s proc timer would kill the
// whole link long after this has cleanly skipped the lost exchange.
#define OP_TIMEOUT_SECS 10

static void prv_op_complete(void);
static void prv_request(uint8_t mask);
static int prv_srcp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);

static uint16_t s_conn;
static uint16_t s_cgm_start, s_cgm_end;
static uint16_t s_h_measurement, s_h_feature, s_h_racp;
static uint16_t s_idd_start, s_idd_end, s_h_srcp;
static uint16_t s_h_status_changed;

// Reassembly buffer for a (decrypted) CGM Measurement record. The record's byte 0 is its total
// length, so accumulate decrypted fragments until we have that many bytes.
static uint8_t s_rec[64];
static uint8_t s_rec_len;

// Reassembly buffer for a (decrypted) SRCP IOB response. Unlike the CGM record there is no byte-0
// length prefix -- the response is a single short indication starting with opcode 0x03FC on the
// 780G, complete once the 7-byte mandatory prefix is present.
static uint8_t s_srcp[24];
static uint8_t s_srcp_len;

// Distinguishes a genuinely new sensor reading from a re-poll of the same one. The CGM record's
// Time Offset (bytes 4-5, minutes since session start) is the only new-reading signal available --
// the value alone is not, since consecutive readings are often identical. Without this the BG
// timestamp would advance on every 60 s poll, so a stalled sensor would look permanently fresh on
// the watchface, and the graph would fill with duplicate points.
static uint16_t s_last_offset;
static bool s_have_offset;
static uint32_t s_reading_ts;  // wall-clock time we first saw the current reading

// Decode an IEEE-11073 SFLOAT (MedFloat16) to an integer mg/dL. Returns INT32_MIN for the
// NaN/NRes/Inf sentinels (no usable value). Glucose normally has exponent 0.
static int32_t prv_decode_medfloat16(uint16_t raw) {
  uint16_t m12 = raw & 0x0FFF;
  if (m12 == 0x07FF || m12 == 0x0800 || m12 == 0x0801 || m12 == 0x07FE || m12 == 0x0802) {
    return INT32_MIN;
  }
  int exp = (raw >> 12) & 0x0F;
  if (exp & 0x8) exp -= 0x10;
  int32_t mant = raw & 0x0FFF;
  if (mant & 0x800) mant -= 0x1000;
  for (; exp > 0; exp--) mant *= 10;
  for (; exp < 0; exp++) mant /= 10;
  return mant;
}

static void prv_parse_and_show(void) {
  // Mandatory prefix: size(1) | flags(1) | glucose SFLOAT(2) | time offset(2).
  if (s_rec_len < 6 || s_rec[0] != s_rec_len) {
    char line[32];
    snprintf(line, sizeof(line), "bad CGM rec len=%u sz=%u", s_rec_len, s_rec[0]);
    minimed_sake_log(line);
    return;
  }
  uint16_t raw = (uint16_t)(s_rec[2] | (s_rec[3] << 8));
  int32_t mgdl = prv_decode_medfloat16(raw);
  char line[32];
  if (mgdl == INT32_MIN) {
    // Warmup / no usable value. Forget the tracked offset: a sentinel run usually means a new
    // sensor session, whose offsets restart from zero and could otherwise happen to land on the
    // previous session's last value -- which would read as a re-poll and pair a fresh reading with
    // an hours-old timestamp.
    s_have_offset = false;
    minimed_sake_log("SG: no value (warmup?)");
    return;
  }
  // mg/dL -> mmol/L to one decimal, rounded. Uses 18.0182 (not the textbook 18.0156): the bridge's
  // GlucoseFormat picked this constant specifically so the rounded value matches the Medtronic
  // pump's own display (differs at rounding boundaries, e.g. 100 mg/dL -> 5.5, not 5.6). Scaled
  // integer math (no float printf on the watch); +90091 = 180182/2 for round-half-up.
  int32_t tenths = (mgdl * 100000 + 90091) / 180182;

  const uint16_t offset = (uint16_t)(s_rec[4] | (s_rec[5] << 8));
  const bool is_new = (!s_have_offset || offset != s_last_offset);
  if (is_new) {
    s_last_offset = offset;
    s_have_offset = true;
    s_reading_ts = (uint32_t)rtc_get_time();
    minimed_sake_sender_add_graph_point(s_reading_ts, mgdl);
    snprintf(line, sizeof(line), "*** BG %ld.%ld mmol/L ***", (long)(tenths / 10),
             (long)(tenths % 10));
  } else {
    // Same reading re-polled. Worth a line so the log still shows the link is alive, and the age
    // makes a stalled sensor obvious instead of looking like fresh data. Clamp at 0 rather than
    // letting an RTC step backwards print a nonsense six-digit age.
    const uint32_t now = (uint32_t)rtc_get_time();
    const uint32_t age_min = (now > s_reading_ts) ? (now - s_reading_ts) / 60 : 0;
    snprintf(line, sizeof(line), "BG %ld.%ld same %lum", (long)(tenths / 10), (long)(tenths % 10),
             (unsigned long)age_min);
  }
  minimed_sake_log(line);

  char bg_str[12];
  snprintf(bg_str, sizeof(bg_str), "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
  // Forward to the watchface (no-op if it isn't running). Timestamped when the reading first
  // appeared, not now, so the watchface's "N min ago" reflects the sensor, not our poll.
  minimed_sake_sender_send_bg(bg_str, s_reading_ts);
}

// Parse a reassembled SRCP IOB response and forward it to the watchface. On a parse failure log
// the leading bytes so an on-watch capture shows exactly what the pump returned (the first HW use
// of encrypt-for-pump could reveal a framing/E2E surprise -- see PROGRESS.md risk register).
static void prv_parse_iob(void) {
  int32_t iob_mu;
  if (!minimed_iob_parse_response(s_srcp, s_srcp_len, &iob_mu)) {
    char line[32];
    snprintf(line, sizeof(line), "IOB bad %u:%02x%02x%02x%02x", s_srcp_len, s_srcp[0], s_srcp[1],
             s_srcp_len > 2 ? s_srcp[2] : 0, s_srcp_len > 3 ? s_srcp[3] : 0);
    minimed_sake_log(line);
    return;
  }
  // Round milliunits to 0.1 IU. Integer math (no float printf on the watch).
  int32_t tenths = (iob_mu + 50) / 100;
  char line[32];
  snprintf(line, sizeof(line), "*** IOB %ld.%ld U ***", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_log(line);

  char iob_str[12];  // matches bg_str sizing; the sender clamps to its own IOB_STR_MAX
  snprintf(iob_str, sizeof(iob_str), "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_sender_send_iob(iob_str);  // forward to the watchface (no-op if it isn't running)
}

// Feed an inbound pump notification/indication. Returns true if consumed (a CGM char we own).
bool minimed_sake_read_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len) {
  if (s_h_measurement != 0 && attr_handle == s_h_measurement) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("CGM decrypt failed");
      return true;
    }
    if (s_rec_len + plain_len > sizeof(s_rec)) {
      s_rec_len = 0;  // overflow guard; abandon this frame
    }
    memcpy(s_rec + s_rec_len, plain, plain_len);
    s_rec_len += plain_len;
    if (s_rec_len >= 1 && s_rec_len >= s_rec[0]) {
      prv_parse_and_show();
      s_rec_len = 0;
    }
    return true;
  }
  if (s_h_status_changed != 0 && attr_handle == s_h_status_changed) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("0x101 decrypt failed");
      PBL_LOG_INFO("SAKE: 0x101 decrypt failed (%u bytes on the wire)", (unsigned)len);
      return true;
    }

    // The pump's push channel. React like the bridge: targeted read(s) for the bits we display,
    // then queue a Reset Status for EVERYTHING received -- the pump latches each bit until
    // reset, so unlatching is what makes the next change indicate at all. The union survives an
    // in-flight exchange; one reset then covers a whole burst (a fingerstick fires ~5
    // indications in 15 s).
    const uint64_t flags = minimed_idd_flags_parse(plain, plain_len);

    // The full flag word (incl. continuation bits) stays logged: the higher bits are still
    // being characterised (Documentation/idd-service.md notes observation contradicting some
    // documented names), and parse is host-tested to round-trip, so this replaces v39's
    // raw-bytes line without losing information.
    char line[32];
    snprintf(line, sizeof(line), "0x101 %08x%08x", (unsigned)(flags >> 32), (unsigned)flags);
    minimed_sake_log(line);
    PBL_LOG_INFO("SAKE: 0x101 push flags=0x%08x%08x (%u plaintext bytes)",
                 (unsigned)(flags >> 32), (unsigned)flags, (unsigned)plain_len);

    uint8_t req = 0;
    if (flags & MINIMED_IDD_FLAG_NEW_CGM) req |= PEND_CGM;
    if (s_h_srcp != 0) {
      if (flags & MINIMED_IDD_FLAG_IOB) req |= PEND_IOB;
      s_reset_flags |= flags;
      req |= PEND_RESET;  // no SRCP char would mean no reset possible; fallback still covers us
    }

    if (!s_push_mode) {
      s_push_mode = true;
      minimed_sake_log("push mode (6m fallback)");
    }
    // Re-arm the dead-man: an indication is proof push is alive.
    ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(FALLBACK_AFTER_SECS * 1000));

    if (req != 0) prv_request(req);
    return true;
  }
  if (s_h_racp != 0 && attr_handle == s_h_racp) {
    // Success is the common case and stays quiet so the log keeps scrolling BG readings; only an
    // unexpected RACP response is worth a line.
    bool ok = (len == sizeof(RACP_REPORT_SUCCESS) &&
               memcmp(data, RACP_REPORT_SUCCESS, len) == 0);
    if (!ok) {
      minimed_sake_log("RACP unexpected resp");
    }
    // Either way the CGM exchange is over. The serialiser then issues whatever is pending
    // (an IOB read queued with this poll, or a Reset Status). Note ops don't strictly need
    // serialising for NimBLE's sake -- gattc ops queue FIFO (BLE_GATT_MAX_PROCS=8) rather than
    // returning BLE_HS_EBUSY as an older comment here claimed -- but the 30 s unresponsive timer
    // starts at *queue* time, and the two reassembly buffers are single-exchange.
    if (s_op == PEND_CGM) {
      if (s_push_mode) {
        // A completed CGM exchange also proves the link; keep the dead-man from re-firing
        // right after a fallback-driven poll.
        ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(FALLBACK_AFTER_SECS * 1000));
      }
      prv_op_complete();
    }
    return true;
  }
  if (s_h_srcp != 0 && attr_handle == s_h_srcp) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("SRCP decrypt failed");
      return true;
    }
    if (s_op == PEND_RESET) {
      // The whole response is one short indication: the generic SRCP Response Code, expected
      // 03 03 0c 03 <result> (opcode 0x0303, echoed request 0x030C, result). Format not yet
      // HW-confirmed, so log the raw bytes; update Documentation/idd-service.md once seen.
      char line[32];
      snprintf(line, sizeof(line), "rst resp %u:%02x%02x%02x%02x%02x", plain_len,
               plain_len > 0 ? plain[0] : 0, plain_len > 1 ? plain[1] : 0,
               plain_len > 2 ? plain[2] : 0, plain_len > 3 ? plain[3] : 0,
               plain_len > 4 ? plain[4] : 0);
      minimed_sake_log(line);
      prv_op_complete();
      return true;
    }
    if (s_op != PEND_IOB) {
      minimed_sake_log("SRCP unsolicited");
      return true;
    }
    if (s_srcp_len + plain_len > sizeof(s_srcp)) {
      s_srcp_len = 0;  // overflow guard; abandon this frame
    }
    memcpy(s_srcp + s_srcp_len, plain, plain_len);
    s_srcp_len += plain_len;
    // No byte-0 length prefix here (unlike the CGM record): the IOB response is a single short
    // indication that starts with opcode 0x03FC, complete at the 7-byte mandatory prefix.
    if (s_srcp_len >= 7) {
      prv_parse_iob();
      s_srcp_len = 0;
      prv_op_complete();
    }
    return true;
  }
  return false;
}

static int prv_racp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP write err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    // No terminating indication will come for a failed write; skip the exchange now rather
    // than stalling the serialiser until the op timeout.
    if (s_op == PEND_CGM) prv_op_complete();
  }
  return 0;
}

static void prv_op_complete(void) {
  ble_npl_callout_stop(&s_op_timeout_co);
  s_op = 0;
  if (s_pending != 0) {
    ble_npl_callout_reset(&s_dispatch_co, ble_npl_time_ms_to_ticks32(DISPATCH_DELAY_MS));
  }
}

// Queue work and kick the dispatcher. Callers guard on the handles they need (PEND_IOB and
// PEND_RESET require s_h_srcp != 0), so the dispatcher never has to skip a queued op.
static void prv_request(uint8_t mask) {
  s_pending |= mask;
  if (s_op == 0) {
    ble_npl_callout_reset(&s_dispatch_co, ble_npl_time_ms_to_ticks32(DISPATCH_DELAY_MS));
  }
}

// Issue the highest-priority pending exchange. Reads before reset (data lands ASAP; one reset
// then covers a whole indication burst). On a failed issue, complete immediately -- no
// indication will terminate an exchange that never started.
static void prv_dispatch_cb(struct ble_npl_event *ev) {
  char line[32];
  if (s_op != 0) return;  // in flight; prv_op_complete re-kicks
  if (s_pending & PEND_CGM) {
    s_pending &= ~PEND_CGM;
    s_op = PEND_CGM;
    s_rec_len = 0;  // reassembly reset at issue time, not in a free-running poll
    int rc = ble_gattc_write_flat(s_conn, s_h_racp, RACP_REPORT_LAST_RECORD,
                                  sizeof(RACP_REPORT_LAST_RECORD), prv_racp_write_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "RACP write rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_IOB) {
    s_pending &= ~PEND_IOB;
    s_op = PEND_IOB;
    uint8_t enc[sizeof(SRCP_GET_IOB) + 3];  // SeqCrypt appends a 1-byte counter + 2-byte MAC
    uint16_t enc_len = 0;
    if (!minimed_sake_encrypt(SRCP_GET_IOB, sizeof(SRCP_GET_IOB), enc, &enc_len)) {
      minimed_sake_log("IOB encrypt failed");
      prv_op_complete();
      return;
    }
    s_srcp_len = 0;
    int rc = ble_gattc_write_flat(s_conn, s_h_srcp, enc, enc_len, prv_srcp_write_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "SRCP write rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_RESET) {
    s_pending &= ~PEND_RESET;
    s_op = PEND_RESET;
    uint8_t plain[sizeof(SRCP_RESET_STATUS) + 6];
    memcpy(plain, SRCP_RESET_STATUS, sizeof(SRCP_RESET_STATUS));
    const uint16_t flags_len =
        minimed_idd_flags_encode(s_reset_flags, plain + sizeof(SRCP_RESET_STATUS));
    s_reset_flags = 0;  // an indication landing mid-exchange starts a fresh union
    uint8_t enc[sizeof(plain) + 3];
    uint16_t enc_len = 0;
    if (!minimed_sake_encrypt(plain, sizeof(SRCP_RESET_STATUS) + flags_len, enc, &enc_len)) {
      minimed_sake_log("rst encrypt failed");
      prv_op_complete();
      return;
    }
    s_srcp_len = 0;
    int rc = ble_gattc_write_flat(s_conn, s_h_srcp, enc, enc_len, prv_srcp_write_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "rst write rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_op_complete();
      return;
    }
  } else {
    return;  // nothing pending
  }
  ble_npl_callout_reset(&s_op_timeout_co, ble_npl_time_ms_to_ticks32(OP_TIMEOUT_SECS * 1000));
}

// A lost terminating indication must not wedge the serialiser (fallback polls dispatch through
// it too, so a wedge would mean "no data", not "stale data"). Drop the exchange and move on.
static void prv_op_timeout_cb(struct ble_npl_event *ev) {
  char line[32];
  snprintf(line, sizeof(line), "op timeout 0x%02x", s_op);
  minimed_sake_log(line);
  s_rec_len = 0;
  s_srcp_len = 0;
  s_op = 0;
  if (s_pending != 0) {
    ble_npl_callout_reset(&s_dispatch_co, ble_npl_time_ms_to_ticks32(DISPATCH_DELAY_MS));
  }
}

static void prv_poll_timer_cb(struct ble_npl_event *ev) {
  if (s_push_mode) minimed_sake_log("fallback poll");
  prv_request(PEND_CGM | (s_h_srcp != 0 ? PEND_IOB : 0));
  const uint32_t secs = s_push_mode ? FALLBACK_AFTER_SECS : POLL_INTERVAL_SECS;
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(secs * 1000));
}

// Begin the continuous CGM poll. IOB rides each poll only if the IDD SRCP char was found
// (s_h_srcp != 0); a missing/failed IDD discovery leaves BG working, just without IOB.
static void prv_start_polling(void) {
  minimed_sake_log(s_h_srcp != 0 ? "polling BG + IOB" : "polling BG only");
  prv_request(PEND_CGM | (s_h_srcp != 0 ? PEND_IOB : 0));
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
  // Push subscription, deliberately LAST and deliberately fire-and-forget. Everything that
  // matters (BG, IOB) is already polling by this point, so a failure here -- or no indication
  // ever arriving -- just leaves the 60 s poll running; push mode only engages on the first
  // actual indication (see the 0x101 branch of minimed_sake_read_handle_notify).
  if (s_h_status_changed != 0) {
    static const uint8_t indicate[] = {0x02, 0x00};
    const int rc = ble_gattc_write_flat(s_conn, s_h_status_changed + 1, indicate, sizeof(indicate),
                                        NULL, NULL);
    char line[32];
    snprintf(line, sizeof(line), "0x101 sub rc=%d", rc);
    minimed_sake_log(line);
    PBL_LOG_INFO("SAKE: subscribed IDD Status Changed (0x101): rc=%d", rc);
  } else {
    PBL_LOG_INFO("SAKE: no IDD Status Changed (0x101) characteristic found");
  }
}

static int prv_srcp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP write err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    // No terminating indication will come for a failed write; skip the exchange now rather
    // than stalling the serialiser until the op timeout.
    if (s_op == PEND_IOB || s_op == PEND_RESET) prv_op_complete();
  }
  return 0;
}

static int prv_sub_srcp_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    s_h_srcp = 0;  // give up on IOB, keep BG
  }
  prv_start_polling();
  return 0;
}

static int prv_disc_idd_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg) {
  char line[32];
  if (error->status == 0 && chr) {
    // The SRCP char is 128-bit vendor, so match by full UUID (not ble_uuid_u16).
    if (ble_uuid_cmp(&chr->uuid.u, &s_idd_srcp_uuid.u) == 0) {
      s_h_srcp = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_status_changed_uuid.u) == 0) {
      s_h_status_changed = chr->val_handle;  // subscribed after polling starts; see prv_start_polling
    }
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    if (s_h_srcp == 0) {
      minimed_sake_log("no IDD SRCP chr");
      prv_start_polling();  // BG still works without IOB
      return 0;
    }
    // Subscribe SRCP indications (CCCD = value handle + 1, same as RACP on this pump).
    static const uint8_t indicate[] = {0x02, 0x00};
    int rc = ble_gattc_write_flat(s_conn, s_h_srcp + 1, indicate, sizeof(indicate),
                                  prv_sub_srcp_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "SRCP sub rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      s_h_srcp = 0;
      prv_start_polling();
    }
    return 0;
  }
  snprintf(line, sizeof(line), "IDD chr disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  s_h_srcp = 0;
  prv_start_polling();
  return 0;
}

static int prv_disc_idd_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *service, void *arg) {
  char line[32];
  if (error->status == 0 && service) {
    s_idd_start = service->start_handle;
    s_idd_end = service->end_handle;
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    if (s_idd_start == 0) {
      minimed_sake_log("no IDD svc 0x100");
      prv_start_polling();  // BG still works without IOB
      return 0;
    }
    int rc = ble_gattc_disc_all_chrs(s_conn, s_idd_start, s_idd_end, prv_disc_idd_chr_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "IDD chr disc rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_start_polling();
    }
    return 0;
  }
  snprintf(line, sizeof(line), "IDD svc disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  prv_start_polling();
  return 0;
}

static int prv_sub_racp_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  // CGM chars subscribed. Extend the setup chain with IDD-service discovery (for IOB); polling
  // starts once that resolves (or immediately falls back to BG-only if the IDD service is absent).
  minimed_sake_log("discovering IDD svc...");
  int rc = ble_gattc_disc_svc_by_uuid(s_conn, &s_idd_svc_uuid.u, prv_disc_idd_svc_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "IDD svc disc rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
    prv_start_polling();  // couldn't even start IDD discovery; keep BG
  }
  return 0;
}

static int prv_sub_meas_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "meas sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  // Subscribe to RACP indications (CCCD = value handle + 1 for these regular 3-handle chars).
  static const uint8_t indicate[] = {0x02, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_racp + 1, indicate, sizeof(indicate),
                                prv_sub_racp_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP sub rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
  return 0;
}

static int prv_read_feature_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  char line[32];
  if (error->status != 0) {
    snprintf(line, sizeof(line), "feat read err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  uint16_t n = (attr && attr->om) ? attr->om->om_len : 0;
  const uint8_t *d = (attr && attr->om) ? attr->om->om_data : NULL;
  snprintf(line, sizeof(line), "CGM feat %u:%02x %02x", n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0);
  minimed_sake_log(line);

  if (s_h_measurement == 0 || s_h_racp == 0) {
    minimed_sake_log("missing meas/RACP chr");
    return 0;
  }
  // Subscribe to CGM Measurement notifications (CCCD = value handle + 1). Chaining the RACP write
  // behind these subscribe write-responses guarantees notifications are effective first.
  static const uint8_t notify[] = {0x01, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_measurement + 1, notify, sizeof(notify),
                                prv_sub_meas_cb, NULL);
  if (rc != 0) {
    snprintf(line, sizeof(line), "meas sub rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
  return 0;
}

static int prv_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg) {
  char line[32];
  if (error->status == 0 && chr) {
    uint16_t u = (chr->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&chr->uuid.u) : 0;
    if (u == CGM_MEASUREMENT_UUID) {
      s_h_measurement = chr->val_handle;
    } else if (u == CGM_FEATURE_UUID) {
      s_h_feature = chr->val_handle;
    } else if (u == RACP_UUID) {
      s_h_racp = chr->val_handle;
    }
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    snprintf(line, sizeof(line), "chrs: m=%u f=%u r=%u", s_h_measurement, s_h_feature, s_h_racp);
    minimed_sake_log(line);
    if (s_h_feature != 0) {
      int rc = ble_gattc_read(s_conn, s_h_feature, prv_read_feature_cb, NULL);
      if (rc != 0) {
        snprintf(line, sizeof(line), "feat read rc=0x%04x", (uint16_t)rc);
        minimed_sake_log(line);
      }
    } else {
      minimed_sake_log("no CGM feature chr!");
    }
    return 0;
  }
  snprintf(line, sizeof(line), "chr disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  return 0;
}

static int prv_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
  char line[32];
  if (error->status == 0 && service) {
    s_cgm_start = service->start_handle;
    s_cgm_end = service->end_handle;
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    if (s_cgm_start == 0) {
      minimed_sake_log("no CGM svc 181F!");
      return 0;
    }
    int rc = ble_gattc_disc_all_chrs(s_conn, s_cgm_start, s_cgm_end, prv_disc_chr_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "chr disc rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
    }
    return 0;
  }
  snprintf(line, sizeof(line), "svc disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  return 0;
}

// Runs on the BT host task a beat after the handshake, so GATT-client procedures aren't started
// synchronously inside the SAKE-port write callback that completed the handshake.
static void prv_read_kickoff(struct ble_npl_event *ev) {
  minimed_sake_log("discovering CGM svc...");
  const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(CGM_SERVICE_UUID);
  int rc = ble_gattc_disc_svc_by_uuid(s_conn, &svc_uuid.u, prv_disc_svc_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "svc disc rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
}

void minimed_sake_read_init(void) {
  ble_npl_callout_init(&s_read_co, nimble_port_get_dflt_eventq(), prv_read_kickoff, NULL);
  ble_npl_callout_init(&s_poll_co, nimble_port_get_dflt_eventq(), prv_poll_timer_cb, NULL);
  ble_npl_callout_init(&s_dispatch_co, nimble_port_get_dflt_eventq(), prv_dispatch_cb, NULL);
  ble_npl_callout_init(&s_op_timeout_co, nimble_port_get_dflt_eventq(), prv_op_timeout_cb, NULL);
}

void minimed_sake_read_start(uint16_t conn_handle) {
  ble_npl_callout_stop(&s_poll_co);
  ble_npl_callout_stop(&s_dispatch_co);
  ble_npl_callout_stop(&s_op_timeout_co);
  s_conn = conn_handle;
  s_cgm_start = s_cgm_end = 0;
  s_h_measurement = s_h_feature = s_h_racp = 0;
  s_idd_start = s_idd_end = s_h_srcp = 0;
  s_h_status_changed = 0;  // re-discovered per connection; a stale handle could alias a new one
  s_rec_len = 0;
  s_srcp_len = 0;
  s_pending = 0;
  s_op = 0;
  s_reset_flags = 0;
  s_push_mode = false;  // a reconnect re-subscribes and must re-prove push
  // s_last_offset/s_have_offset deliberately survive a reconnect: the pump's Time Offset is
  // monotonic within a sensor session, so keeping it means the first read after a brief dropout is
  // recognised as the reading we already have, rather than being re-timestamped and re-plotted. A
  // new sensor session restarts the offset, which reads as a new value anyway.
  ble_npl_callout_reset(&s_read_co, ble_npl_time_ms_to_ticks32(250));
}

void minimed_sake_read_stop(void) {
  ble_npl_callout_stop(&s_read_co);
  ble_npl_callout_stop(&s_poll_co);
  ble_npl_callout_stop(&s_dispatch_co);
  ble_npl_callout_stop(&s_op_timeout_co);
}
