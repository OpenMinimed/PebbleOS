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

// SRCP "Get Insulin On Board" request: little-endian opcode 0x03F3. NOT E2E-CRC-wrapped -- the
// 780G leaves E2E protection off for the IDD service (Documentation/idd-service.md), matching the
// bridge's srcpGet which does not append a CRC. SAKE-encrypted before it goes on the wire.
static const uint8_t SRCP_GET_IOB[] = {0xF3, 0x03};

// Re-poll the latest record on this cadence. The sensor updates ~every 5 min; polling faster just
// re-shows the current value and keeps the on-watch reading fresh within one interval.
#define POLL_INTERVAL_SECS 60

static struct ble_npl_callout s_read_co;
static struct ble_npl_callout s_poll_co;
static struct ble_npl_callout s_iob_co;  // deferred SRCP IOB read, chained after each CGM poll
static uint16_t s_conn;
static uint16_t s_cgm_start, s_cgm_end;
static uint16_t s_h_measurement, s_h_feature, s_h_racp;
static uint16_t s_idd_start, s_idd_end, s_h_srcp;

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
    // PUSH PROBE (temporary, remove once answered): see prv_do_poll. Logged before decrypting so a
    // frame still counts even if it fails to decrypt -- arrival timing is the whole question here.
    PBL_LOG_INFO("SAKE: CGM notify %u bytes (push probe)", (unsigned)len);
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
  if (s_h_racp != 0 && attr_handle == s_h_racp) {
    // Success is the common case and stays quiet so the log keeps scrolling BG readings; only an
    // unexpected RACP response is worth a line.
    bool ok = (len == sizeof(RACP_REPORT_SUCCESS) &&
               memcmp(data, RACP_REPORT_SUCCESS, len) == 0);
    if (!ok) {
      minimed_sake_log("RACP unexpected resp");
    } else if (s_h_srcp != 0) {
      // The CGM poll just finished (its terminating indication is this one). Chain the IOB read
      // ~200 ms later, off this notify context and after the CGM gattc procedure has fully
      // completed -- NimBLE allows only one outstanding client op, so serializing CGM->IOB avoids
      // BLE_HS_EBUSY and keeps the shared inbound cipher counter in order.
      ble_npl_callout_reset(&s_iob_co, ble_npl_time_ms_to_ticks32(200));
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
  }
  return 0;
}

// Issue one RACP "report last stored record"; the record arrives via measurement notifications.
static void prv_do_poll(void) {
  s_rec_len = 0;
  // PUSH PROBE (temporary, remove once answered): pairs with the "CGM notify" line below. If the
  // pump ever notifies a measurement WITHOUT us asking, a notify will appear in the flash log far
  // from any poll -- and event-driven BG then needs no new code at all, because we are already
  // subscribed to 0x2AA7. If every notify hugs a poll, the pump only answers RACP and we need the
  // IDD Status Changed (0x101) push machinery instead.
  PBL_LOG_INFO("SAKE: RACP poll write (push probe)");
  int rc = ble_gattc_write_flat(s_conn, s_h_racp, RACP_REPORT_LAST_RECORD,
                                sizeof(RACP_REPORT_LAST_RECORD), prv_racp_write_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP write rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
}

static void prv_poll_timer_cb(struct ble_npl_event *ev) {
  prv_do_poll();
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
}

// Begin the continuous CGM poll. IOB rides each poll only if the IDD SRCP char was found
// (s_h_srcp != 0); a missing/failed IDD discovery leaves BG working, just without IOB.
// Peripheral latency to ask the pump for once the link goes idle. The pump dictates the connection
// parameters and never renegotiates, so without this the watch's radio wakes every connection
// interval around the clock (measured: 125 ms, latency 0) -- the largest steady drain on the link.
// Latency N lets us skip up to N connection events when we have nothing to send, cutting wakeups
// ~(N+1)x. It costs nothing in responsiveness that matters here: our own polls are
// peripheral-initiated and go out at the next event regardless, and a pump-initiated notification
// is delayed by at most N intervals, which is irrelevant against a 5-minute sensor cadence.
//
// HW result 2026-07-26: latency 4 was REJECTED with HCI 0x3B (unacceptable connection parameters),
// even though the request kept the pump's own interval and supervision timeout and cleared the
// spec constraint with a wide margin. So this is now a probe rather than an optimisation: 1 is the
// smallest ask that still halves the wakeups, and it distinguishes "the pump dislikes that value"
// from "the pump refuses peripheral-initiated updates at all". If 1 is refused too, delete this and
// go via the NOS service instead (see below). Other untried variation: offering a range rather than
// itvl_min == itvl_max, which some centrals insist on.
#define DESIRED_SLAVE_LATENCY 1

// Ask the pump to let us idle. Deliberately keeps the pump's own interval and supervision timeout
// and changes only the latency: the narrowest possible request, so there is least to reject.
//
// This is the plain BLE route (an L2CAP parameter-update request, since we are the peripheral).
// Medtronic also define a sanctioned one -- the NOS service's "Observation Mode" write carries
// min/max interval, slave latency and supervision timeout (../Documentation/nos-service.md), and is
// presumably what the official app uses. It is not the first thing to try: it needs its own service
// discovery and a SAKE-encrypted write, and the doc lists every field's unit as "???", so we would
// be guessing. Try the standard mechanism first and read the result out of the flash log; if the
// pump rejects it, NOS is the justified next step -- and worth documenting upstream either way.
static void prv_request_slave_latency(void) {
  struct ble_gap_conn_desc d;
  if (ble_gap_conn_find(s_conn, &d) != 0) {
    return;
  }

  // The link dies if a whole supervision window can elapse while we are legitimately silent, so the
  // spec requires (latency + 1) * interval * 2 < supervision_timeout. In native units (interval
  // 1.25 ms, timeout 10 ms) that reduces to (latency + 1) * itvl < sv * 4. Derive the ceiling from
  // what the pump actually chose rather than assuming the measured 125 ms / 3000 ms holds forever.
  uint16_t max_latency = 0;
  if (d.conn_itvl > 0) {
    const uint32_t limit = ((uint32_t)d.supervision_timeout * 4) / d.conn_itvl;
    max_latency = (limit > 1) ? (uint16_t)(limit - 1) : 0;
  }
  const uint16_t latency = (DESIRED_SLAVE_LATENCY < max_latency) ? DESIRED_SLAVE_LATENCY
                                                                 : max_latency;
  if (latency == 0 || d.conn_latency >= latency) {
    return;  // nothing to gain (no headroom, or the pump already gave us latency)
  }

  struct ble_gap_upd_params p = {
      .itvl_min = d.conn_itvl,
      .itvl_max = d.conn_itvl,
      .latency = latency,
      .supervision_timeout = d.supervision_timeout,
  };
  const int rc = ble_gap_update_params(s_conn, &p);

  // Log either way: a reject is harmless (the link keeps the pump's parameters) but we want to know
  // which happened, and the acceptance shows up separately as a "Connection parameters updated"
  // line from advert.c.
  char line[32];
  snprintf(line, sizeof(line), "lat req %u rc=%d", (unsigned)latency, rc);
  minimed_sake_log(line);
  // INFO, not DBG: the default log level is INFO, so a DBG line would never reach a flash dump --
  // and reading this back afterwards is the entire point. Fires once per pump connection.
  PBL_LOG_INFO("SAKE: requested slave latency %u (itvl=%u sv=%u): rc=%d",
               (unsigned)latency, (unsigned)d.conn_itvl, (unsigned)d.supervision_timeout, rc);
}

static void prv_start_polling(void) {
  minimed_sake_log(s_h_srcp != 0 ? "polling BG + IOB" : "polling BG only");
  prv_do_poll();
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
  // Only now, with discovery finished: a latent link is what we want from here on, but asking any
  // earlier would have slowed the service/characteristic discovery that just ran.
  prv_request_slave_latency();
}

static int prv_srcp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP write err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
  }
  return 0;
}

// Deferred (post-CGM-poll) SRCP "get IOB": SAKE-encrypt the request and write it to the SRCP value
// handle. The response comes back as an SRCP indication (handled in minimed_sake_read_handle_notify).
static void prv_iob_read_cb(struct ble_npl_event *ev) {
  uint8_t enc[sizeof(SRCP_GET_IOB) + 3];  // SeqCrypt appends a 1-byte counter + 2-byte MAC
  uint16_t enc_len = 0;
  if (!minimed_sake_encrypt(SRCP_GET_IOB, sizeof(SRCP_GET_IOB), enc, &enc_len)) {
    minimed_sake_log("IOB encrypt failed");
    return;
  }
  s_srcp_len = 0;  // reset reassembly for this exchange
  int rc = ble_gattc_write_flat(s_conn, s_h_srcp, enc, enc_len, prv_srcp_write_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP write rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
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
  ble_npl_callout_init(&s_iob_co, nimble_port_get_dflt_eventq(), prv_iob_read_cb, NULL);
}

void minimed_sake_read_start(uint16_t conn_handle) {
  ble_npl_callout_stop(&s_poll_co);
  ble_npl_callout_stop(&s_iob_co);
  s_conn = conn_handle;
  s_cgm_start = s_cgm_end = 0;
  s_h_measurement = s_h_feature = s_h_racp = 0;
  s_idd_start = s_idd_end = s_h_srcp = 0;
  s_rec_len = 0;
  s_srcp_len = 0;
  // s_last_offset/s_have_offset deliberately survive a reconnect: the pump's Time Offset is
  // monotonic within a sensor session, so keeping it means the first read after a brief dropout is
  // recognised as the reading we already have, rather than being re-timestamped and re-plotted. A
  // new sensor session restarts the offset, which reads as a new value anyway.
  ble_npl_callout_reset(&s_read_co, ble_npl_time_ms_to_ticks32(250));
}

void minimed_sake_read_stop(void) {
  ble_npl_callout_stop(&s_read_co);
  ble_npl_callout_stop(&s_poll_co);
  ble_npl_callout_stop(&s_iob_co);
}
