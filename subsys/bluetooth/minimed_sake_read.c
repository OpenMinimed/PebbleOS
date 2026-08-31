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
#include "minimed_annunciation.h"
#include "minimed_idd_flags.h"
#include "minimed_iob.h"
#include "popups/minimed_alert_popup.h"
#include "minimed_sake_sender.h"
#include "minimed_status.h"
#include "minimed_sake_service.h"
#include "popups/minimed_sake_spike_ui.h"
#include <pbl/logging/logging.h>

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
// IDD Status 0x102 (read, SAKE-encrypted): therapy/operational state, reservoir, sensor state --
// the record behind the watchface status line (v41). Parsed in minimed_status.{c,h}.
static const ble_uuid128_t s_idd_status_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00);
// IDD History Data 0x108 (notify, SAKE-encrypted per fragment): the event log, read via the IDD
// service's own RACP (SIG 0x2A52, plaintext, write + indicate). Used for pump annunciations
// (alarms/alerts): the 0x101 annunciation bit only says "changed"; the reason lives here as
// Annunciation Consolidated records (minimed_annunciation.{c,h}).
static const ble_uuid128_t s_idd_hist_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00);

// SRCP "Get Insulin On Board" request: little-endian opcode 0x03F3. NOT E2E-CRC-wrapped -- the
// 780G leaves E2E protection off for the IDD service (Documentation/idd-service.md), matching the
// bridge's srcpGet which does not append a CRC. SAKE-encrypted before it goes on the wire.
static const uint8_t SRCP_GET_IOB[] = {0xF3, 0x03};

// SRCP "Reset Status": little-endian opcode 0x030C + the flag field to clear, encoded exactly
// like 0x101's (minimed_idd_flags_encode). Clears indication latches only -- the pump keeps each
// 0x101 bit latched until reset, so this is what makes a second indication ever arrive.
static const uint8_t SRCP_RESET_STATUS[] = {0x0C, 0x03};

// SRCP "Get Therapy Algorithm States": little-endian opcode 0x03FD -> response 0x03FE. Carries
// the SmartGuard shield/readiness and temp-target minutes, none of which are in IDD Status.
static const uint8_t SRCP_GET_TAS[] = {0xFD, 0x03};

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
// exchanges (CGM poll, SRCP IOB read, IDD Status read, SRCP TAS read, SRCP Reset Status) each
// span a request plus its response, share the reassembly buffers, and can be triggered
// asynchronously by 0x101 pushes -- so exactly one is in flight at a time. s_op holds the
// in-flight op's PEND_ bit (0 = idle) and doubles as the SRCP-response disambiguator: the same
// char carries the IOB response (complete at >= 7 bytes), the TAS response, and the short Reset
// Status response.
#define PEND_CGM 0x01
#define PEND_IOB 0x02
#define PEND_RESET 0x04
#define PEND_STATUS 0x08
#define PEND_TAS 0x10
#define PEND_ANNUNC 0x20
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

// 780G sensor display range, 2.8-22.2 mmol/L. Off-scale readings graph at the edge they crossed.
#define SG_FLOOR_MGDL 50
#define SG_CEILING_MGDL 400

// Pump battery (SIG Battery Level 0x2A19, plaintext read). Hourly samples to characterise its
// granularity -- Documentation PR #2 claims it is very coarse (only 50 and 100 % ever seen, from
// bridge-era reads); the evidence log is gone, so re-gather. Plaintext single read on its own
// char: no SAKE cipher involvement and no shared buffers, so it bypasses the exchange serialiser.
// The pump's Device Information Service (0x180A), read once per boot. Plaintext, so no SAKE and
// no shared buffers. Identifies which pump and firmware produced a log -- the baseline for
// comparing against a newer, Simplera-Sync-capable pump -- and the whole set is listed as never
// captured in OpenMinimed's todo.md, so the values are also a doc contribution. The nine
// characteristics are those documented in Documentation/pump-services.md.
#define DEVINFO_READ_DELAY_SECS 20
static struct ble_npl_callout s_devinfo_co;
static bool s_devinfo_read;
static uint8_t s_devinfo_idx;

// Binary fields are logged as hex: System ID is 8 bytes, PnP ID 7, and the IEEE 11073 regulatory
// certification list is a structured blob -- none of them are text.
static const struct {
  uint16_t uuid;
  const char *name;
  bool hex;
} s_devinfo_chrs[] = {
    {0x2A29, "manufacturer", false},
    {0x2A24, "model", false},
    {0x2A25, "serial", false},
    {0x2A27, "hardware revision", false},
    {0x2A26, "firmware revision", false},
    {0x2A28, "software revision", false},
    {0x2A23, "system id", true},
    {0x2A50, "pnp id", true},
    {0x2A2A, "ieee regulatory cert", true},
};
#define DEVINFO_CHR_COUNT (sizeof(s_devinfo_chrs) / sizeof(s_devinfo_chrs[0]))

#define BATTERY_LEVEL_UUID 0x2A19
#define BATTERY_READ_INTERVAL_SECS (60 * 60)
#define BATTERY_FIRST_READ_DELAY_SECS 30
static struct ble_npl_callout s_battery_co;

static void prv_op_complete(void);
static void prv_request(uint8_t mask);
static void prv_status_publish_if_done(uint8_t completed_op);
static int prv_srcp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);
static int prv_idd_status_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg);
static int prv_idd_racp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg);

static uint16_t s_conn;
static uint16_t s_cgm_start, s_cgm_end;
static uint16_t s_h_measurement, s_h_feature, s_h_racp;
static uint16_t s_idd_start, s_idd_end, s_h_srcp;
static uint16_t s_h_status_changed;
static uint16_t s_h_idd_status;
static uint16_t s_h_idd_racp, s_h_hist;

// Latest parsed status pair, one-shot per read cycle: invalidated after each publish so a failed
// read next cycle is not papered over with the previous cycle's fields (mirrors the bridge
// passing null for a failed read). The *label* state that survives across cycles lives in
// minimed_status.c.
static MinimedIddStatus s_idd_st;
static MinimedTas s_tas;

// Re-compose the status string every minute while a countdown/count-up label is active
// (WARM-UP / TEMP TARGET / SUSPENDED), so it ticks on the watchface. Local AppMessage only --
// no BLE traffic.
#define STATUS_TICK_SECS 60
static struct ble_npl_callout s_status_tick_co;

// Reassembly buffer for a (decrypted) CGM Measurement record. The record's byte 0 is its total
// length, so accumulate decrypted fragments until we have that many bytes.
static uint8_t s_rec[64];
static uint8_t s_rec_len;

// Reassembly buffer for a (decrypted) SRCP IOB response. Unlike the CGM record there is no byte-0
// length prefix -- the response is a single short indication starting with opcode 0x03FC on the
// 780G, complete once the 7-byte mandatory prefix is present.
static uint8_t s_srcp[24];
static uint8_t s_srcp_len;

// Reassembly buffer for one (decrypted) IDD History Data record. Records have no length prefix;
// the pump fills notifications to the ATT cap, so a fragment shorter than ATT_MTU-3 on the wire
// ends the record (the bridge's reassembler rule), with a flush at the RACP terminal indication
// covering a record that is an exact multiple of the fragment size.
static uint8_t s_hist[64];
static uint8_t s_hist_len;

// Annunciation cursor. s_annunc_seq is the newest history sequence number already processed;
// reads ask for everything after it. Re-baselined per connection via a "report last record"
// exchange that never notifies -- alarms raised while disconnected are deliberately dropped (the
// pump alarms audibly; the watch only mirrors alarms it is connected for). s_annunc_baseline
// marks the in-flight exchange as that baseline read.
static uint32_t s_annunc_seq;
static bool s_annunc_have;      // baseline done; catch-up reads may notify
static bool s_annunc_baseline;  // the in-flight PEND_ANNUNC exchange is the baseline read
static bool s_annunc_seen;      // the in-flight exchange delivered >= 1 record

// Latest BG as shown on the watchface ("4.2" mmol/L, "LO"/"HI"; "" while the pump has no valid
// glucose). Alert notifications carry it as their body -- a low alert without the number is
// half the information.
static char s_last_bg_str[12];

// Recently notified annunciation instance ids: the same annunciation can be re-logged with an
// updated status (semantics not fully characterised), and a raise must buzz exactly once.
// 0xFFFF = empty slot. Deliberately survives reconnects.
static uint16_t s_annunc_ids[8] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
static uint8_t s_annunc_ids_next;

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
    // Raw bytes to flash: which sentinel the pump uses (and that records flow at all) during
    // warm-up / transmitter charging is undocumented.
    PBL_LOG_INFO("SAKE: CGM sentinel rec %02x %02x %02x %02x %02x %02x",
                 s_rec[0], s_rec[1], s_rec[2], s_rec[3], s_rec[4], s_rec[5]);
    return;
  }
  const uint16_t offset = (uint16_t)(s_rec[4] | (s_rec[5] << 8));
  const bool is_new = (!s_have_offset || offset != s_last_offset);

  if (mgdl == 0 || (is_new && mgdl >= SG_CEILING_MGDL)) {
    // Raw-record capture for the still-uncharacterised off-scale encodings: 0 mg/dL confirmed on
    // HW 2026-08-16 during SG-below and "sensor updating"; what SG-above sends is an assumption
    // (0 like below?), so a real HIGH capture is what would correct the branch below.
    // side: 0 = neither, 1 = SG below, 2 = SG above (max 7 conversions per PBL_LOG).
    PBL_LOG_INFO("SAKE: CGM edge rec %02x %02x %02x %02x %02x %02x side=%d",
                 s_rec[0], s_rec[1], s_rec[2], s_rec[3], s_rec[4], s_rec[5],
                 minimed_status_sg_below() ? 1 : (minimed_status_sg_above() ? 2 : 0));
  }
  if (mgdl == 0) {
    // 0 mg/dL is a marker, not a reading: the pump sends it (with advancing time offsets) while
    // the SG is off-scale or the sensor has no glucose ("sensor updating", ...). Never show it
    // as a number and never graph it as 0.
    const bool below = minimed_status_sg_below();
    const bool above = minimed_status_sg_above();
    if (!below && !above) {
      // No glucose to show: leave the last BG aging, the status band explains why. The offset is
      // deliberately NOT consumed, so if this is really an off-scale onset raced ahead of the
      // status read, the same record is re-judged as new once the status catches up (<=1 cycle).
      minimed_sake_log("SG: 0 marker, skip");
      return;
    }
    // Off-scale: show LO/HI like the pump, graph at the scale edge that was crossed ("at or
    // beyond"), timestamped fresh -- the sensor is reporting, just out of range.
    if (is_new) {
      s_last_offset = offset;
      s_have_offset = true;
      s_reading_ts = (uint32_t)rtc_get_time();
      minimed_sake_sender_add_graph_point(s_reading_ts, below ? SG_FLOOR_MGDL : SG_CEILING_MGDL);
    }
    minimed_sake_log(below ? "*** BG LO ***" : "*** BG HI ***");
    strcpy(s_last_bg_str, below ? "LO" : "HI");
    minimed_sake_sender_send_bg(s_last_bg_str, s_reading_ts);
    return;
  }

  // mg/dL -> mmol/L to one decimal, rounded. Uses 18.0182 (not the textbook 18.0156): the bridge's
  // GlucoseFormat picked this constant specifically so the rounded value matches the Medtronic
  // pump's own display (differs at rounding boundaries, e.g. 100 mg/dL -> 5.5, not 5.6). Scaled
  // integer math (no float printf on the watch); +90091 = 180182/2 for round-half-up.
  int32_t tenths = (mgdl * 100000 + 90091) / 180182;

  if (is_new) {
    s_last_offset = offset;
    s_have_offset = true;
    s_reading_ts = (uint32_t)rtc_get_time();
    minimed_sake_sender_add_graph_point(s_reading_ts, mgdl);
    snprintf(line, sizeof(line), "*** BG %ld.%ld mmol/L ***", (long)(tenths / 10),
             (long)(tenths % 10));
    // Flash mirror (the ring lines don't reach flash): when readings resume after a sensor
    // state, and at what offset, is otherwise invisible in a dump.
    PBL_LOG_INFO("SAKE: BG new %ld mg/dL offset=%u", (long)mgdl, (unsigned)offset);
  } else {
    // Same reading re-polled. Worth a line so the log still shows the link is alive, and the age
    // makes a stalled sensor obvious instead of looking like fresh data. Clamp at 0 rather than
    // letting an RTC step backwards print a nonsense six-digit age.
    const uint32_t now = (uint32_t)rtc_get_time();
    const uint32_t age_min = (now > s_reading_ts) ? (now - s_reading_ts) / 60 : 0;
    snprintf(line, sizeof(line), "BG %ld.%ld same %lum", (long)(tenths / 10), (long)(tenths % 10),
             (unsigned long)age_min);
    if (minimed_status_bg_invalid()) {
      // The pump has no current glucose (per the status read) and this is just the last stored
      // record re-polled: keep the "---" the status publish sent rather than flipping the stale
      // number back on. A genuinely NEW reading (branch above) always shows.
      minimed_sake_log(line);
      return;
    }
  }
  minimed_sake_log(line);

  snprintf(s_last_bg_str, sizeof(s_last_bg_str), "%ld.%ld", (long)(tenths / 10),
           (long)(tenths % 10));
  // Forward to the watchface (no-op if it isn't running). Timestamped when the reading first
  // appeared, not now, so the watchface's "N min ago" reflects the sensor, not our poll.
  minimed_sake_sender_send_bg(s_last_bg_str, s_reading_ts);
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
  // Raw milliunits to the flash log: minute-cadence IOB traces from routine dumps are the data
  // for recovering the pump's decay curve (true-IOB investigation, 2026-08-17).
  PBL_LOG_INFO("SAKE: IOB %ld mu", (long)iob_mu);

  // Round milliunits to 0.1 IU. Integer math (no float printf on the watch).
  int32_t tenths = (iob_mu + 50) / 100;
  char line[32];
  snprintf(line, sizeof(line), "*** IOB %ld.%ld U ***", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_log(line);

  char iob_str[12];  // matches bg_str sizing; the sender clamps to its own IOB_STR_MAX
  snprintf(iob_str, sizeof(iob_str), "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_sender_send_iob(iob_str);  // forward to the watchface (no-op if it isn't running)
}

static bool prv_annunc_already_notified(uint16_t id) {
  const size_t n = sizeof(s_annunc_ids) / sizeof(s_annunc_ids[0]);
  for (size_t i = 0; i < n; i++) {
    if (s_annunc_ids[i] == id) return true;
  }
  s_annunc_ids[s_annunc_ids_next++ % n] = id;
  return false;
}

// One reassembled history record is complete: advance the cursor, and post a notification for a
// new, un-silenced annunciation raise (never during the baseline read).
static void prv_annunc_record_done(void) {
  MinimedAnnunciation a;
  const MinimedAnnuncRecord r = minimed_annunciation_parse_record(s_hist, s_hist_len, &a);
  const uint8_t rec_len = s_hist_len;
  s_hist_len = 0;
  if (r == MinimedAnnuncRecordBad) {
    PBL_LOG_INFO("SAKE: bad hist rec len=%u %02x %02x %02x %02x", (unsigned)rec_len, s_hist[0],
                 s_hist[1], s_hist[2], s_hist[3]);
    return;
  }
  s_annunc_seen = true;
  if (a.seq > s_annunc_seq) s_annunc_seq = a.seq;
  if (r != MinimedAnnuncRecordYes) return;

  // Every annunciation to flash, notified or not: this is also the field log that grows the
  // code/status catalog (docs/PUMP-DATA.md table).
  PBL_LOG_INFO("SAKE: annunc type=0x%03x id=%u status=0x%02x sil=%d seq=%lu base=%d",
               (unsigned)a.type, (unsigned)a.id, (unsigned)a.status, (int)a.silenced,
               (unsigned long)a.seq, (int)s_annunc_baseline);
  if (s_annunc_baseline) return;
  if (a.silenced) return;  // the pump raised it quietly (alert settings); mirror that choice
  if (prv_annunc_already_notified(a.id)) return;

  char name[28];
  const char *known = minimed_annunciation_name(a.type);
  if (known != NULL) {
    snprintf(name, sizeof(name), "%s", known);
  } else {
    snprintf(name, sizeof(name), "Pump alert 0x%03x", (unsigned)a.type);
  }
  minimed_sake_log(name);
  // Body: the alert name with the latest BG in parens, e.g. "Alert before low (4.2)". The BG is
  // at most one 5-min cycle old, and the CGM read dispatches before this one on the same push,
  // so on a fresh alert it is usually seconds old; dropped entirely when the pump has no value.
  char body[48];
  if (s_last_bg_str[0] != '\0') {
    snprintf(body, sizeof(body), "%s (%s)", name, s_last_bg_str);
  } else {
    snprintf(body, sizeof(body), "%s", name);
  }
  minimed_alert_popup_push("MiniMed", body);
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
    if (flags & (MINIMED_IDD_FLAG_THERAPY_CONTROL | MINIMED_IDD_FLAG_OPERATIONAL |
                 MINIMED_IDD_FLAG_THERAPY_ALGORITHM)) {
      // Suspend/resume, an operational-state transition (reservoir-change walk: bit 1 is rare,
      // unlike bit 2 which rides every microbolus), or SmartGuard/temp-target changed: re-read
      // the status pair (bridge bits + bit 1).
      req |= (s_h_idd_status != 0 ? PEND_STATUS : 0) | (s_h_srcp != 0 ? PEND_TAS : 0);
    }
    if (s_h_idd_racp != 0 && s_h_hist != 0 &&
        ((flags & MINIMED_IDD_FLAG_ANNUNCIATION) || !s_annunc_have)) {
      // An alarm was raised or cleared: read the history records behind it. Until the baseline
      // read has succeeded, any push doubles as a retry of it.
      req |= PEND_ANNUNC;
    }
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
  if (s_h_hist != 0 && attr_handle == s_h_hist) {
    uint8_t plain[64];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("hist decrypt failed");
      return true;
    }
    if (s_hist_len + plain_len > sizeof(s_hist)) {
      s_hist_len = 0;  // overflow guard; abandon this record
    }
    memcpy(s_hist + s_hist_len, plain, plain_len);
    s_hist_len += plain_len;
    // No length prefix: a wire fragment shorter than the ATT cap ends the record (the pump fills
    // notifications to the cap; the bridge's reassembler uses the same rule). An exact-multiple
    // record is flushed at the RACP terminal instead.
    const uint16_t mtu = ble_att_mtu(s_conn);
    const uint16_t att_max = (mtu > 3) ? (mtu - 3) : 20;
    if (len < att_max) prv_annunc_record_done();
    // A long catch-up read can outlive the 10 s op timer; each fragment is proof of progress.
    if (s_op == PEND_ANNUNC) {
      ble_npl_callout_reset(&s_op_timeout_co, ble_npl_time_ms_to_ticks32(OP_TIMEOUT_SECS * 1000));
    }
    return true;
  }
  if (s_h_idd_racp != 0 && attr_handle == s_h_idd_racp) {
    // Plaintext terminal indication: 0f 0f 33 f0 = success, 0f 0f 33 06 = no records (an empty
    // window is a clean result, not an error).
    if (s_hist_len != 0) prv_annunc_record_done();  // exact-multiple flush
    const bool ok = (len >= 4 && data[0] == 0x0F && data[2] == 0x33 &&
                     (data[3] == 0xF0 || data[3] == 0x06));
    if (!ok) {
      char line[32];
      snprintf(line, sizeof(line), "IDD RACP resp %02x%02x%02x%02x", len > 0 ? data[0] : 0,
               len > 1 ? data[1] : 0, len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
      minimed_sake_log(line);
    }
    if (s_op == PEND_ANNUNC) {
      if (s_annunc_baseline && s_annunc_seen) {
        s_annunc_have = true;
        PBL_LOG_INFO("SAKE: annunc baseline seq=%lu", (unsigned long)s_annunc_seq);
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
    if (s_op == PEND_TAS) {
      // Single short indication (max ~14 plaintext bytes); no reassembly needed.
      if (!minimed_status_parse_tas(plain, plain_len, &s_tas)) {
        minimed_sake_log("TAS bad resp");
      } else {
        PBL_LOG_INFO("SAKE: tas auto=%d shield=%02x ready=%02x tt=%u", (int)s_tas.has_auto_mode,
                     s_tas.shield, s_tas.readiness, (unsigned)s_tas.temp_target_min);
      }
      prv_status_publish_if_done(PEND_TAS);
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

static int prv_idd_racp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "IDD RACP wr err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    // No terminating indication will come for a failed write; skip the exchange.
    if (s_op == PEND_ANNUNC) prv_op_complete();
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

// Called when a STATUS or TAS exchange finishes (success, failure, or timeout). The two are
// always requested as a pair when both chars exist; publish once the pair's other read is no
// longer queued -- ops are serialised, so "not pending" means "done or never requested". A read
// that failed leaves its struct invalid, and the mapping just skips its clauses.
static void prv_status_publish_if_done(uint8_t completed_op) {
  const uint8_t other = (completed_op == PEND_STATUS) ? PEND_TAS : PEND_STATUS;
  if (s_pending & other) return;
  const uint32_t now = (uint32_t)rtc_get_time();
  minimed_status_update(&s_idd_st, &s_tas, now);
  char label[20];
  if (minimed_status_compose(now, label, sizeof(label))) {
    minimed_sake_sender_send_status(label);
    char line[32];
    snprintf(line, sizeof(line), "st: %s", label[0] != '\0' ? label : "(normal)");
    minimed_sake_log(line);
    PBL_LOG_INFO("SAKE: status label '%s' bg_invalid=%d", label,
                 (int)minimed_status_bg_invalid());
    if (minimed_status_bg_invalid()) {
      // The pump has no valid glucose right now (warm-up, signal lost, ...): blank the BG
      // immediately, stamped now so the watchface shows a current "---" like the pump does,
      // instead of an old number with a climbing age. The next real reading overwrites it.
      minimed_sake_sender_send_bg("---", now);
      s_last_bg_str[0] = '\0';  // no valid glucose: alert notifications drop the BG body
    }
  }
  s_idd_st.valid = false;
  s_tas.valid = false;
}

// IDD Status (0x102) is a plain encrypted READ -- the one exchange that completes in its own
// GATT callback rather than via an indication.
static int prv_idd_status_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg) {
  if (s_op != PEND_STATUS) return 0;  // late/stale callback; a newer op owns the buffers now
  char line[32];
  if (error->status == 0 && attr && attr->om) {
    // Single mbuf fragment is safe here: the value is 12 bytes on the wire (9 + SeqCrypt 3).
    const uint16_t n = attr->om->om_len;
    const uint8_t *d = attr->om->om_data;
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(d, n, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("st decrypt failed");
    } else if (!minimed_status_parse_idd(plain, plain_len, &s_idd_st)) {
      snprintf(line, sizeof(line), "st bad len=%u", plain_len);
      minimed_sake_log(line);
    } else {
      PBL_LOG_INFO("SAKE: status t=%02x o=%02x fl=%02x conn=%02x msg=%02x res=%ld mu",
                   s_idd_st.therapy, s_idd_st.operational, s_idd_st.flags, s_idd_st.sensor_conn,
                   s_idd_st.sensor_msg, (long)s_idd_st.reservoir_mu);
    }
  } else {
    snprintf(line, sizeof(line), "st read err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
  }
  prv_status_publish_if_done(PEND_STATUS);
  prv_op_complete();
  return 0;
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
  } else if (s_pending & PEND_ANNUNC) {
    s_pending &= ~PEND_ANNUNC;
    s_op = PEND_ANNUNC;
    s_hist_len = 0;
    s_annunc_baseline = !s_annunc_have;
    s_annunc_seen = false;
    // IDD RACP is plaintext. Baseline: report last record (33 69 0f) to learn the newest
    // sequence number. Catch-up: report within range (33 5a 0f + min/max u32 LE) from the
    // cursor; the open-ended max is a HW question -- the terminal response will say if the
    // pump insists on a real upper bound.
    uint8_t req[11] = {0x33, 0x69, 0x0F};
    uint16_t req_len = 3;
    if (!s_annunc_baseline) {
      req[1] = 0x5A;
      const uint32_t lo = s_annunc_seq + 1;
      req[3] = (uint8_t)lo;
      req[4] = (uint8_t)(lo >> 8);
      req[5] = (uint8_t)(lo >> 16);
      req[6] = (uint8_t)(lo >> 24);
      req[7] = req[8] = req[9] = req[10] = 0xFF;
      req_len = 11;
    }
    int rc = ble_gattc_write_flat(s_conn, s_h_idd_racp, req, req_len, prv_idd_racp_write_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "IDD RACP write rc=0x%04x", (uint16_t)rc);
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
  } else if (s_pending & PEND_STATUS) {
    s_pending &= ~PEND_STATUS;
    s_op = PEND_STATUS;
    int rc = ble_gattc_read(s_conn, s_h_idd_status, prv_idd_status_read_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "st read rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_status_publish_if_done(PEND_STATUS);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_TAS) {
    s_pending &= ~PEND_TAS;
    s_op = PEND_TAS;
    uint8_t enc[sizeof(SRCP_GET_TAS) + 3];
    uint16_t enc_len = 0;
    if (!minimed_sake_encrypt(SRCP_GET_TAS, sizeof(SRCP_GET_TAS), enc, &enc_len)) {
      minimed_sake_log("TAS encrypt failed");
      prv_status_publish_if_done(PEND_TAS);
      prv_op_complete();
      return;
    }
    s_srcp_len = 0;
    int rc = ble_gattc_write_flat(s_conn, s_h_srcp, enc, enc_len, prv_srcp_write_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "TAS write rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
      prv_status_publish_if_done(PEND_TAS);
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
  const uint8_t op = s_op;
  s_rec_len = 0;
  s_srcp_len = 0;
  s_hist_len = 0;
  s_op = 0;
  if (op == PEND_STATUS || op == PEND_TAS) {
    // The timed-out read stays invalid; publish whatever the pair's other half delivered.
    prv_status_publish_if_done(op);
  }
  if (s_pending != 0) {
    ble_npl_callout_reset(&s_dispatch_co, ble_npl_time_ms_to_ticks32(DISPATCH_DELAY_MS));
  }
}

// Everything a full poll reads, gated on the handles that were actually discovered.
static uint8_t prv_full_poll_mask(void) {
  return PEND_CGM | (s_h_srcp != 0 ? (PEND_IOB | PEND_TAS) : 0) |
         (s_h_idd_status != 0 ? PEND_STATUS : 0) |
         // Annunciation baseline rides the poll until it succeeds; after that only 0x101
         // annunciation pushes trigger reads.
         (s_h_idd_racp != 0 && s_h_hist != 0 && !s_annunc_have ? PEND_ANNUNC : 0);
}

static void prv_devinfo_read_next(void);

// v57 hard-faulted twice inside picolibc's %s conversion, with the fault correlating to this
// sweep. Unproven, but the plausible mechanism is stack exhaustion on the NimBLE host task: v57
// put 73 bytes of buffers in this callback's frame and then called PBL_LOG, which is itself
// stack-hungry (logging.c guards the same hazard via prv_use_default_log_msg). So the whole line
// is composed into one *static* buffer here and logged with a single %s, leaving this frame
// nearly empty. The sweep is serialised, so one shared buffer is safe.
static char s_devinfo_line[80];

// read_by_uuid fires once per matching attribute, then once more with BLE_HS_EDONE. NimBLE runs
// one GATT procedure at a time per connection, so the next characteristic is chained off EDONE
// rather than issuing all nine at once.
static int prv_devinfo_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  const uint8_t idx = (uint8_t)(uintptr_t)arg;

  if (error->status == BLE_HS_EDONE) {
    s_devinfo_idx = idx + 1;
    prv_devinfo_read_next();
    return 0;
  }
  if (error->status != 0 || !attr || !attr->om || attr->om->om_len < 1) {
    // Advance past a field the pump will not give us; v57 returned here without advancing, which
    // stalled the sweep on that characteristic and re-read it every session.
    snprintf(s_devinfo_line, sizeof(s_devinfo_line), "%s read err=0x%04x", s_devinfo_chrs[idx].name,
             (uint16_t)error->status);
    PBL_LOG_INFO("SAKE: pump %s", s_devinfo_line);
    s_devinfo_idx = idx + 1;
    prv_devinfo_read_next();
    return 0;
  }

  const uint16_t n = attr->om->om_len;
  int off = snprintf(s_devinfo_line, sizeof(s_devinfo_line), "%s ", s_devinfo_chrs[idx].name);
  if (off < 0 || (unsigned)off >= sizeof(s_devinfo_line)) {
    return 0;
  }

  if (s_devinfo_chrs[idx].hex) {
    for (uint16_t i = 0; i < n && (unsigned)off + 3 < sizeof(s_devinfo_line); i++) {
      off += snprintf(&s_devinfo_line[off], 3, "%02x", attr->om->om_data[i]);
    }
  } else {
    unsigned room = sizeof(s_devinfo_line) - off - 1;
    uint16_t len = n > room ? (uint16_t)room : n;
    memcpy(&s_devinfo_line[off], attr->om->om_data, len);
    while (len > 0 && s_devinfo_line[off + len - 1] == '\0') len--;  // trim a trailing NUL
    s_devinfo_line[off + len] = '\0';
  }
  PBL_LOG_INFO("SAKE: pump %s", s_devinfo_line);
  return 0;
}

// Read by UUID over the whole handle range, like the battery read: saves discovering the service,
// and these 16-bit UUIDs cannot collide with the vendor 128-bit ones.
static void prv_devinfo_read_next(void) {
  if (s_devinfo_idx >= DEVINFO_CHR_COUNT) {
    s_devinfo_read = true;  // whole sweep done; latch so it stays once per boot
    return;
  }
  const uint8_t idx = s_devinfo_idx;
  const ble_uuid16_t uuid = BLE_UUID16_INIT(s_devinfo_chrs[idx].uuid);
  int rc = ble_gattc_read_by_uuid(s_conn, 0x0001, 0xffff, &uuid.u, prv_devinfo_read_cb,
                                  (void *)(uintptr_t)idx);
  if (rc != 0) {
    // Abandon the sweep without latching, so the next session retries from the start.
    PBL_LOG_INFO("SAKE: pump %s read rc=0x%04x", s_devinfo_chrs[idx].name, (uint16_t)rc);
    s_devinfo_idx = 0;
  }
}

// Once per boot, not per session: these only change across a pump firmware update or a pump swap,
// and the pump reconnects often enough that a per-session sweep would be log spam.
static void prv_devinfo_timer_cb(struct ble_npl_event *ev) {
  if (s_devinfo_read) {
    return;
  }
  s_devinfo_idx = 0;
  prv_devinfo_read_next();
}

// read_by_uuid fires once per matching attribute, then once more with BLE_HS_EDONE.
static int prv_battery_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  if (error->status == 0 && attr && attr->om && attr->om->om_len >= 1) {
    PBL_LOG_INFO("SAKE: pump battery %u pct", (unsigned)attr->om->om_data[0]);
  } else if (error->status != BLE_HS_EDONE) {
    PBL_LOG_INFO("SAKE: pump battery read err=0x%04x", (uint16_t)error->status);
  }
  return 0;
}

// Read by UUID over the whole handle range: saves discovering the Battery service, and the GST
// battery (vendor 128-bit 0x400) can't collide with a 16-bit match.
static void prv_battery_timer_cb(struct ble_npl_event *ev) {
  const ble_uuid16_t uuid = BLE_UUID16_INIT(BATTERY_LEVEL_UUID);
  int rc = ble_gattc_read_by_uuid(s_conn, 0x0001, 0xffff, &uuid.u, prv_battery_read_cb, NULL);
  if (rc != 0) {
    PBL_LOG_INFO("SAKE: pump battery read rc=0x%04x", (uint16_t)rc);
  }
  ble_npl_callout_reset(&s_battery_co,
                        ble_npl_time_ms_to_ticks32(BATTERY_READ_INTERVAL_SECS * 1000));
}

static void prv_poll_timer_cb(struct ble_npl_event *ev) {
  if (s_push_mode) minimed_sake_log("fallback poll");
  prv_request(prv_full_poll_mask());
  const uint32_t secs = s_push_mode ? FALLBACK_AFTER_SECS : POLL_INTERVAL_SECS;
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(secs * 1000));
}

// While a countdown/count-up status is showing, re-compose and re-send it every minute so it
// ticks on the watchface. Purely local (AppMessage injection) -- costs no BLE traffic.
static void prv_status_tick_cb(struct ble_npl_event *ev) {
  if (minimed_status_ticking()) {
    char label[20];
    if (minimed_status_compose((uint32_t)rtc_get_time(), label, sizeof(label))) {
      minimed_sake_sender_send_status(label);
    }
  }
  ble_npl_callout_reset(&s_status_tick_co, ble_npl_time_ms_to_ticks32(STATUS_TICK_SECS * 1000));
}

// Begin the continuous CGM poll. IOB rides each poll only if the IDD SRCP char was found
// (s_h_srcp != 0); a missing/failed IDD discovery leaves BG working, just without IOB.
static void prv_start_polling(void) {
  minimed_sake_log(s_h_srcp != 0 ? "polling BG + IOB" : "polling BG only");
  prv_request(prv_full_poll_mask());
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
  ble_npl_callout_reset(&s_status_tick_co, ble_npl_time_ms_to_ticks32(STATUS_TICK_SECS * 1000));
  ble_npl_callout_reset(&s_battery_co,
                        ble_npl_time_ms_to_ticks32(BATTERY_FIRST_READ_DELAY_SECS * 1000));
  ble_npl_callout_reset(&s_devinfo_co,
                        ble_npl_time_ms_to_ticks32(DEVINFO_READ_DELAY_SECS * 1000));
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
    if (s_op == PEND_TAS) prv_status_publish_if_done(PEND_TAS);
    if (s_op == PEND_IOB || s_op == PEND_RESET || s_op == PEND_TAS) prv_op_complete();
  }
  return 0;
}

// Annunciation subscriptions (IDD RACP indicate, then History Data notify), chained before
// polling starts. Any failure zeroes both handles -- no alerts, BG/IOB/status unaffected.
static void prv_annunc_give_up(const char *what, uint16_t code) {
  char line[32];
  snprintf(line, sizeof(line), "%s 0x%04x", what, code);
  minimed_sake_log(line);
  s_h_idd_racp = 0;
  s_h_hist = 0;
  prv_start_polling();
}

static int prv_sub_hist_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    prv_annunc_give_up("hist sub err", (uint16_t)error->status);
    return 0;
  }
  prv_start_polling();
  return 0;
}

static int prv_sub_idd_racp_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    prv_annunc_give_up("IDD RACP sub err", (uint16_t)error->status);
    return 0;
  }
  static const uint8_t notify[] = {0x01, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_hist + 1, notify, sizeof(notify), prv_sub_hist_cb,
                                NULL);
  if (rc != 0) prv_annunc_give_up("hist sub rc", (uint16_t)rc);
  return 0;
}

static void prv_sub_annunc(void) {
  if (s_h_idd_racp == 0 || s_h_hist == 0) {
    s_h_idd_racp = 0;
    s_h_hist = 0;
    minimed_sake_log("no IDD RACP/hist chr");
    prv_start_polling();
    return;
  }
  static const uint8_t indicate[] = {0x02, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_idd_racp + 1, indicate, sizeof(indicate),
                                prv_sub_idd_racp_cb, NULL);
  if (rc != 0) prv_annunc_give_up("IDD RACP sub rc", (uint16_t)rc);
}

static int prv_sub_srcp_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    s_h_srcp = 0;  // give up on IOB, keep BG
  }
  prv_sub_annunc();
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
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_status_uuid.u) == 0) {
      s_h_idd_status = chr->val_handle;  // encrypted read; drives the watchface status line
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_hist_uuid.u) == 0) {
      s_h_hist = chr->val_handle;
    } else if (chr->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&chr->uuid.u) == RACP_UUID) {
      // The IDD service has its own RACP (same SIG 0x2A52 as the CGM one, different handle).
      s_h_idd_racp = chr->val_handle;
    }
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    if (s_h_srcp == 0) {
      minimed_sake_log("no IDD SRCP chr");
      prv_sub_annunc();  // BG still works without IOB
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
      prv_sub_annunc();
    }
    return 0;
  }
  snprintf(line, sizeof(line), "IDD chr disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  s_h_srcp = 0;
  prv_sub_annunc();
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
  ble_npl_callout_init(&s_status_tick_co, nimble_port_get_dflt_eventq(), prv_status_tick_cb, NULL);
  ble_npl_callout_init(&s_battery_co, nimble_port_get_dflt_eventq(), prv_battery_timer_cb, NULL);
  ble_npl_callout_init(&s_devinfo_co, nimble_port_get_dflt_eventq(), prv_devinfo_timer_cb, NULL);
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
  s_h_idd_status = 0;
  s_h_idd_racp = 0;
  s_h_hist = 0;
  s_rec_len = 0;
  s_srcp_len = 0;
  s_hist_len = 0;
  // Annunciations re-baseline per connection: alarms raised while disconnected are dropped by
  // design (the pump alarms audibly; the watch mirrors alarms it is connected for). The
  // notified-ids ring deliberately survives, so a re-logged pre-reconnect alarm can't re-buzz.
  s_annunc_have = false;
  s_annunc_seq = 0;
  s_annunc_baseline = false;
  s_annunc_seen = false;
  s_pending = 0;
  s_op = 0;
  s_reset_flags = 0;
  s_push_mode = false;  // a reconnect re-subscribes and must re-prove push
  s_idd_st.valid = false;
  s_tas.valid = false;
  // minimed_status.c state deliberately survives the reconnect (a warm-up countdown keeps
  // counting through a pump dropout); only the per-cycle parse structs reset here.
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
  ble_npl_callout_stop(&s_status_tick_co);
  ble_npl_callout_stop(&s_battery_co);
  ble_npl_callout_stop(&s_devinfo_co);
}
