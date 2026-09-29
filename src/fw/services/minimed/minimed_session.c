/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

// The pump session: after the SAKE handshake, read the pump's CGM and IDD services as a GATT
// client -- the exchange serialiser, the parsers, the predictor and hypo models, and what goes to
// the watchface. Runs on the MiniMed task (minimed_task.c), driven by the events the transport
// (minimed_transport.h) and the timers post; it never calls the Bluetooth stack directly.

#include "minimed_session.h"

#include <stdio.h>
#include <string.h>

#include "drivers/rtc.h"
#include "util/time/time.h"
#include "kernel/kernel_heap.h"
#include "minimed_annunciation.h"
#include "minimed_history.h"
#include "minimed_hypo.h"
#include "minimed_idd_flags.h"
#include "minimed_predict.h"
#include "minimed_iob.h"
#include "minimed_settings.h"
#include "popups/minimed_alert_popup.h"
#include "minimed_sake_sender.h"
#include "minimed_status.h"
#include "minimed_task.h"
#include "minimed_transport.h"
#include "pebble_glucose_protocol.h"
#include "popups/minimed_sake_ui.h"
#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Timers (minimed_task_timer_start). Each fires as a MinimedEventTimer on this task.
enum {
  TimerKickoff,     // a beat after the handshake: start discovery
  TimerPoll,        // 60 s poll, or the 6-minute dead-man once push mode is proven
  TimerWatchdog,    // pump-liveness watchdog (silent-link retoggle)
  TimerDispatch,    // issue the next pending exchange
  TimerOpTimeout,   // unwedge a lost terminating indication
  TimerBattery,
  TimerHeap,        // fixed-interval kernel heap watch
  TimerDevinfo,
  TimerSensorInfo,
  TimerAnnuncProbe,  // start of the Snooze/Confirm probe, then its per-command timeout
  TimerCount
};
_Static_assert(TimerCount <= MINIMED_TIMER_COUNT, "raise MINIMED_TIMER_COUNT");

// GATT operation tags: which request a MinimedEventGattDone answers.
enum {
  TagNone,  // fire-and-forget: no completion event
  TagCgmFeatureRead,
  TagSubMeasurement,
  TagSubCgmRacp,
  TagSubSrcp,
  TagSubIddRacp,
  TagSubHistory,
  TagCgmRacpWrite,
  TagIddRacpWrite,
  TagSrcpWrite,
  TagIddStatusRead,
  TagDevinfoRead,
  TagBatteryRead,
  TagSensorInfoRead,
  TagSensorExpSub,
  TagSessionStartRead,
  TagProbeSubData,
  TagProbeSubCp,
  TagProbeStatusRead,
  TagProbeWrite,
  TagPumpClockRead,
};

// RACP "Report Stored Records: Last Record" and its success response (Bluetooth SIG RACP).
static const uint8_t RACP_REPORT_LAST_RECORD[] = {0x01, 0x06};
static const uint8_t RACP_REPORT_SUCCESS[] = {0x06, 0x00, 0x01, 0x01};

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
// poll timer becomes a dead-man fallback at the bridge's tuned rate. CGM should push every
// ~5 min, so 6 min of silence means push is late or dead -- do one full read and re-arm. A
// silently dead push thus degrades to a 6-minute poll, the bridge's soaked trade-off.
#define FALLBACK_AFTER_SECS (6 * 60)
static bool s_push_mode;  // false until the first indication of this connection proves push


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

// Writes are dispatched off a timer, never straight from a notify/indication: NimBLE sends an
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
static bool s_devinfo_read;
static uint8_t s_devinfo_idx;

// Binary fields are logged as hex: System ID is 8 bytes, PnP ID 7, and the IEEE 11073 regulatory
// certification list is a structured blob -- none of them are text.
static const struct {
  MinimedChr chr;
  const char *name;
  bool hex;
} s_devinfo_chrs[] = {
    {MinimedChrDisManufacturer, "manufacturer", false},
    {MinimedChrDisModel, "model", false},
    {MinimedChrDisSerial, "serial", false},
    {MinimedChrDisHardware, "hardware revision", false},
    {MinimedChrDisFirmware, "firmware revision", false},
    {MinimedChrDisSoftware, "software revision", false},
    {MinimedChrDisSystemId, "system id", true},
    {MinimedChrDisPnpId, "pnp id", true},
    {MinimedChrDisRegulatory, "ieee regulatory cert", true},
};
#define DEVINFO_CHR_COUNT (sizeof(s_devinfo_chrs) / sizeof(s_devinfo_chrs[0]))

#define BATTERY_READ_INTERVAL_SECS (60 * 60)
#define BATTERY_FIRST_READ_DELAY_SECS 30

static void prv_op_complete(void);
static void prv_request(uint8_t mask);
static void prv_backfill_maybe_request(void);
static void prv_refill_if_gap(uint32_t prev_ts, uint32_t new_ts);
static void prv_predict_reading(int32_t mgdl);
static void prv_hypo_check(const MinimedPredictWindow *win);
static void prv_status_publish_if_done(uint8_t completed_op);
static void prv_sensorinfo_read(uint8_t step);
static void prv_sensorinfo_log_value(const char *name, const uint8_t *raw, uint16_t n);
static void prv_annunc_probe_notify(const uint8_t *data, uint16_t len);

// True between LinkUp and LinkDown: events for a link that is gone are dropped.
static bool s_link_up;

// Which characteristics this connection can use: what discovery found, minus any the setup gave
// up on (a failed subscribe drops IOB or alerts, and BG keeps working). Reset per connection.
static bool s_have[MinimedChrCount];
#define HAVE(chr) (s_have[(chr)])

// ---- Sensor-info probe (spike for issue #16) ----
// One chained sweep per connection, ~30 s after polling starts: Session Run Time (0x2AAB),
// Time Of Sensor Expiration (0x0202), IDD Features (0x0104), Session Start Time (0x2AAA).
// Every value is logged raw AND after a decrypt attempt, so the probe itself settles whether
// each characteristic is plaintext or SAKE-encrypted, and its real field width.
#define SENSORINFO_READ_DELAY_SECS 30
static bool s_sensorinfo_done;
// Steps of the sweep; each ends with a chained read of the next.
#define SI_STEP_RUN_TIME 0
#define SI_STEP_EXPIRATION 1
#define SI_STEP_IDD_FEATURES 2
#define SI_STEP_SESSION_START 3
#define SI_STEP_COUNT 4
static uint8_t s_sensorinfo_step;
// Shared line buffers: PBL_LOG is stack-hungry (see the v57 note on the devinfo sweep).
static char s_sensorinfo_line[112];

// Pump-liveness watchdog. If the pump was connected but no traffic arrives for this long, the link
// is presumed silently dead (the controller may never deliver a disconnect), so re-toggle DUAL to
// free the phantom connection slot and re-arm the pump advert. This is the simplest recovery --
// not a diagnostic, hence separate from the Layer 4 'lnk' check.
#define PUMP_WD_NO_TRAFFIC_SECS (15 * 60)
static uint32_t s_last_pump_traffic;  // wall-clock time of the last pump data/exchange completion

// Latest parsed status pair, one-shot per read cycle: invalidated after each publish so a failed
// read next cycle is not papered over with the previous cycle's fields (mirrors the bridge
// passing null for a failed read). The *label* state that survives across cycles lives in
// minimed_status.c.
static MinimedIddStatus s_idd_st;
static MinimedTas s_tas;

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

// Graph backfill: once per connection, after the annunciation baseline and the first CGM read (the
// anchor), read the SG Measurement records covering the visible graph window from the event log
// and hand them to the graph. The log has no time filter, so it is read by sequence number: a
// fixed span behind the newest record, which comfortably covers the window (SG samples share the
// log with basal, bolus and reference-time records). Runs through the PEND_ANNUNC exchange, with
// notifications suppressed as in the baseline read.
//
// The log carries no absolute times. Each sample's time is the pump-clock time of the latest NGP
// Reference Time record before it (logged hourly) plus its own minute offset. The pump clock has no
// time zone, so only differences are used: the newest sample is taken to be the CGM reading the
// watch already showed (stamped s_reading_ts), and older samples sit their pump-clock distance
// behind it.
#define BACKFILL_WINDOW_MIN 240  // the predictor reads 4 hours; the graph keeps what it shows
#define BACKFILL_SEQ_SPAN 300
static bool s_backfill_done;    // this connection's backfill has been issued
static bool s_backfill_wanted;  // the next PEND_ANNUNC exchange is to be the backfill read
static bool s_backfill_run;     // the in-flight PEND_ANNUNC exchange is the backfill read
static MinimedHistClock s_backfill_clock;

// The backfill places the log's samples on the watch clock through one offset, pump clock minus
// watch clock. Normally it comes from the newest sample being the reading the watch just received.
// With no live reading (a sensor warming up right after a change) it comes from the pump's
// Current Time instead, read at that moment, so the hours before the change still reach the graph.
static bool s_pump_clock_known;
static bool s_pump_clock_requested;
static int32_t s_pump_clock_offset;  // pump secs - watch secs, when s_pump_clock_known

// A short live gap (a couple of missed 5-min CGM cycles) while already connected: re-issue the same
// RACP history read the connect-time backfill above uses, so the pump's own log fills the hole
// instead of leaving a permanent break in the graph and the predictor's window. Cooldown bounds how
// often this can fire so a run of gaps (a bad radio patch) doesn't turn into a RACP read on every
// poll; REFILL_GAP_MIN is set above one missed cycle (5 min) with margin for poll jitter.
#define REFILL_GAP_MIN 12
#define REFILL_COOLDOWN_SECS (20 * 60)
static uint32_t s_last_refill_ts;  // rtc seconds of the last live refill trigger; 0 = never
// Off-scale side of the newest SG sample in the pump's event log: 0 none, 1 below, 2 above. The IDD
// status only names the side sometimes, and in one capture never did over 15 minutes of 0 mg/dL
// records, while the log held the below-range code for every one of them.
static uint8_t s_hist_edge;
static uint16_t s_backfill_raw[MINIMED_BACKFILL_MAX_POINTS];
static uint8_t s_backfill_n;  // the newest MINIMED_BACKFILL_MAX_POINTS samples, oldest first
static uint32_t s_backfill_secs[MINIMED_BACKFILL_MAX_POINTS];  // pump clock
static int32_t s_backfill_mgdl[MINIMED_BACKFILL_MAX_POINTS];
// The insulin, meal and basal-rate events of the same read, for the predictor, oldest first.
#define BACKFILL_MAX_EVENTS 64
static uint8_t s_backfill_ne;
static uint32_t s_backfill_esecs[BACKFILL_MAX_EVENTS];
static float s_backfill_evalue[BACKFILL_MAX_EVENTS];
static uint8_t s_backfill_ekind[BACKFILL_MAX_EVENTS];  // MinimedHistEventKind

// The glucose predictor's view of the last 4 hours. Filled from the backfill (which resets it, so
// a reconnect cannot count an event twice) and then live; it predicts only once the backfill has
// given it the insulin and meals of the window, or it would see a body with no history.
static MinimedPredictState s_pred;
static MinimedPredictScore s_pred_score;
static bool s_pred_ready;

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
static int32_t s_reading_mgdl;  // its value, for checking the backfill anchor

// Decode an IEEE-11073 SFLOAT (MedFloat16) to an integer, scaled by `scale` before the exponent is
// applied (e.g. scale=10 keeps one decimal digit instead of truncating it away). Returns INT32_MIN
// for the NaN/NRes/Inf sentinels (no usable value).
static int32_t prv_decode_medfloat16_scaled(uint16_t raw, int32_t scale) {
  uint16_t m12 = raw & 0x0FFF;
  if (m12 == 0x07FF || m12 == 0x0800 || m12 == 0x0801 || m12 == 0x07FE || m12 == 0x0802) {
    return INT32_MIN;
  }
  int exp = (raw >> 12) & 0x0F;
  if (exp & 0x8) exp -= 0x10;
  int32_t mant = raw & 0x0FFF;
  if (mant & 0x800) mant -= 0x1000;
  int32_t val = mant * scale;
  for (; exp > 0; exp--) val *= 10;
  for (; exp < 0; exp++) val /= 10;
  return val;
}

// Decode an IEEE-11073 SFLOAT (MedFloat16) to an integer mg/dL. Glucose normally has exponent 0,
// so scale=1 loses nothing.
static int32_t prv_decode_medfloat16(uint16_t raw) {
  return prv_decode_medfloat16_scaled(raw, 1);
}

// Bucket a CGM trend rate (tenths of mg/dL/min) into one of the protocol's TREND_* arrows. Per the
// golden reference (medtronic_new/PythonPumpConnector cgm/measurement.py, quoting the 780G
// manual): 1 arrow for 1-2 mg/dL/min, 2 arrows for 2-3, 3 arrows for more than 3 -- the pump's own
// display never distinguishes further, so the top bucket maps to DOUBLE_*, not TRIPLE_* (the
// protocol defines TRIPLE_* for other data sources with finer resolution; the pump just doesn't
// have a 4th tier).
static uint8_t prv_trend_arrow_from_rate_tenths(int32_t rate_tenths) {
  static const uint8_t up[] = {TREND_FLAT, TREND_SLANT_UP, TREND_UP, TREND_DOUBLE_UP};
  static const uint8_t down[] = {TREND_FLAT, TREND_SLANT_DOWN, TREND_DOWN, TREND_DOUBLE_DOWN};
  int32_t mag = (rate_tenths < 0) ? -rate_tenths : rate_tenths;
  int32_t level = mag / 10;  // whole mg/dL/min
  if (level > 3) {
    level = 3;  // cap at the table size (DOUBLE_*, the pump's own worst-case bucket)
  }
  return (rate_tenths < 0) ? down[level] : up[level];
}

// CGM Measurement flags bit 0 ("CGM Trend Information present"), per the Bluetooth CGMS spec and
// the golden reference's parse(): bits 0x80/0x40/0x20 gate optional Status/Cal-Temp/Warning
// octets that, when present, each push the trend field's offset one byte further out -- it is
// NOT always right after the mandatory 6-byte prefix.
#define CGM_FLAG_STATUS_PRESENT 0x80
#define CGM_FLAG_CAL_TEMP_PRESENT 0x40
#define CGM_FLAG_WARNING_PRESENT 0x20
#define CGM_FLAG_TREND_INFO_PRESENT 0x01

// Forward this reading's trend (or its absence) to the watchface. Called once per NEW reading,
// same cadence as add_graph_point: a re-poll of an unchanged record must not re-derive or
// re-announce a trend, and a reading that genuinely carries no trend field must clear any
// previously shown arrow rather than let it go stale.
static void prv_forward_trend(uint8_t flags) {
  size_t off = 6;  // end of the mandatory prefix: size(1) flags(1) glucose(2) offset(2)
  if (flags & CGM_FLAG_STATUS_PRESENT) off += 1;
  if (flags & CGM_FLAG_CAL_TEMP_PRESENT) off += 1;
  if (flags & CGM_FLAG_WARNING_PRESENT) off += 1;
  if ((flags & CGM_FLAG_TREND_INFO_PRESENT) && s_rec_len >= off + 2) {
    uint16_t traw = (uint16_t)(s_rec[off] | (s_rec[off + 1] << 8));
    int32_t rate_tenths = prv_decode_medfloat16_scaled(traw, 10);
    if (rate_tenths != INT32_MIN) {
      minimed_sake_sender_send_trend_arrow(true, prv_trend_arrow_from_rate_tenths(rate_tenths));
      return;
    }
  }
  minimed_sake_sender_send_trend_arrow(false, TREND_UNKNOWN);
}

static void prv_parse_and_show(void) {
  // Mandatory prefix: size(1) | flags(1) | glucose SFLOAT(2) | time offset(2).
  if (s_rec_len < 6 || s_rec[0] != s_rec_len) {
    char line[32];
    snprintf(line, sizeof(line), "bad CGM rec len=%u sz=%u", s_rec_len, s_rec[0]);
    minimed_sake_log(line);
    return;
  }
  const uint8_t flags = s_rec[1];
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
    PBL_LOG_INFO("minimed: CGM sentinel rec %02x %02x %02x %02x %02x %02x",
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
    PBL_LOG_INFO("minimed: CGM edge rec %02x %02x %02x %02x %02x %02x side=%d",
                 s_rec[0], s_rec[1], s_rec[2], s_rec[3], s_rec[4], s_rec[5],
                 minimed_status_sg_below() ? 1 : (minimed_status_sg_above() ? 2 : 0));
    PBL_LOG_INFO("minimed: SG history edge=%d", (int)s_hist_edge);
  }
  if (mgdl == 0) {
    // 0 mg/dL is a marker, not a reading: the pump sends it (with advancing time offsets) while
    // the SG is off-scale or the sensor has no glucose ("sensor updating", ...). Never show it
    // as a number and never graph it as 0.
    const bool below = minimed_status_sg_below() || s_hist_edge == 1;
    const bool above = minimed_status_sg_above() || s_hist_edge == 2;
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
      const uint32_t prev_ts = s_reading_ts;
      s_last_offset = offset;
      s_have_offset = true;
      s_reading_ts = (uint32_t)rtc_get_time();
      s_reading_mgdl = below ? SG_FLOOR_MGDL : SG_CEILING_MGDL;
      minimed_sake_sender_add_graph_point(s_reading_ts, s_reading_mgdl);
      prv_predict_reading(s_reading_mgdl);
      prv_forward_trend(flags);
      prv_backfill_maybe_request();
      prv_refill_if_gap(prev_ts, s_reading_ts);
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
    const uint32_t prev_ts = s_reading_ts;
    s_hist_edge = 0;  // a real number: the sensor is back in range
    s_last_offset = offset;
    s_have_offset = true;
    s_reading_ts = (uint32_t)rtc_get_time();
    s_reading_mgdl = mgdl;
    minimed_sake_sender_add_graph_point(s_reading_ts, mgdl);
    prv_predict_reading(mgdl);
    prv_forward_trend(flags);
    prv_backfill_maybe_request();
    prv_refill_if_gap(prev_ts, s_reading_ts);
    snprintf(line, sizeof(line), "*** BG %ld.%ld mmol/L ***", (long)(tenths / 10),
             (long)(tenths % 10));
    // Flash mirror (the ring lines don't reach flash): when readings resume after a sensor
    // state, and at what offset, is otherwise invisible in a dump.
    PBL_LOG_INFO("minimed: BG new %ld mg/dL offset=%u", (long)mgdl, (unsigned)offset);
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
  PBL_LOG_INFO("minimed: IOB %ld mu", (long)iob_mu);

  // Round milliunits to 0.1 IU. Integer math (no float printf on the watch).
  int32_t tenths = (iob_mu + 50) / 100;
  char line[64];
  snprintf(line, sizeof(line), "*** IOB %ld.%ld U ***", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_log(line);

  char iob_str[12];  // matches bg_str sizing; the sender clamps to its own IOB_STR_MAX
  snprintf(iob_str, sizeof(iob_str), "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_sender_send_iob(iob_str);  // forward to the watchface (no-op if it isn't running)

  // Total IOB: the pump's number counts boluses only, so add the basal insulin still active. Only
  // once the backfill has loaded the last hours of delivery, or the basal part would be missing.
  if (s_pred_ready) {
    const int32_t basal_mu = minimed_predict_basal_iob_mu(&s_pred, (uint32_t)rtc_get_time());
    const int32_t total_tenths = (iob_mu + basal_mu + 50) / 100;
    PBL_LOG_INFO("minimed: total IOB %ld mu = pump %ld + basal %ld", (long)(iob_mu + basal_mu),
                 (long)iob_mu, (long)basal_mu);
    snprintf(line, sizeof(line), "tot %ld.%ld=%ld.%ld+%ld.%ld", (long)(total_tenths / 10),
             (long)(total_tenths % 10), (long)(tenths / 10), (long)(tenths % 10),
             (long)((basal_mu + 50) / 1000), (long)(((basal_mu + 50) / 100) % 10));
    minimed_sake_log(line);
    snprintf(iob_str, sizeof(iob_str), "%ld.%ld", (long)(total_tenths / 10),
             (long)(total_tenths % 10));
    minimed_sake_sender_send_total_iob(iob_str);
  }
}

static bool prv_annunc_already_notified(uint16_t id) {
  const size_t n = sizeof(s_annunc_ids) / sizeof(s_annunc_ids[0]);
  for (size_t i = 0; i < n; i++) {
    if (s_annunc_ids[i] == id) return true;
  }
  s_annunc_ids[s_annunc_ids_next++ % n] = id;
  return false;
}

// A history record read during the backfill: keep its event, timed on the pump's clock. The read
// starts hours back, so the oldest are shifted out to keep the newest.
static void prv_backfill_record(uint8_t rec_len) {
  MinimedHistEvent ev;
  if (!minimed_history_decode(&s_backfill_clock, s_hist, rec_len, SG_FLOOR_MGDL, SG_CEILING_MGDL,
                              &ev)) {
    return;
  }
  if (ev.kind == MinimedHistEventRef) return;
  if (ev.kind == MinimedHistEventSg) {
    if (ev.mgdl < 0) return;
    if (s_backfill_n == MINIMED_BACKFILL_MAX_POINTS) {
      memmove(s_backfill_secs, s_backfill_secs + 1, (s_backfill_n - 1) * sizeof(s_backfill_secs[0]));
      memmove(s_backfill_mgdl, s_backfill_mgdl + 1, (s_backfill_n - 1) * sizeof(s_backfill_mgdl[0]));
      memmove(s_backfill_raw, s_backfill_raw + 1, (s_backfill_n - 1) * sizeof(s_backfill_raw[0]));
      s_backfill_n--;
    }
    s_backfill_secs[s_backfill_n] = ev.secs;
    s_backfill_mgdl[s_backfill_n] = ev.mgdl;
    s_backfill_raw[s_backfill_n] = ev.sg;
    s_backfill_n++;
    return;
  }
  if (s_backfill_ne == BACKFILL_MAX_EVENTS) {
    memmove(s_backfill_esecs, s_backfill_esecs + 1, (s_backfill_ne - 1) * sizeof(s_backfill_esecs[0]));
    memmove(s_backfill_evalue, s_backfill_evalue + 1, (s_backfill_ne - 1) * sizeof(s_backfill_evalue[0]));
    memmove(s_backfill_ekind, s_backfill_ekind + 1, (s_backfill_ne - 1) * sizeof(s_backfill_ekind[0]));
    s_backfill_ne--;
  }
  s_backfill_esecs[s_backfill_ne] = ev.secs;
  s_backfill_evalue[s_backfill_ne] = ev.value;
  s_backfill_ekind[s_backfill_ne] = (uint8_t)ev.kind;
  s_backfill_ne++;
}

// Local-time offset of the watch, seconds east of UTC.
static int32_t prv_gmt_offset(uint32_t now) { return (int32_t)(time_utc_to_local((time_t)now) - (time_t)now); }

// A falling low: score whether to treat, with the sugar_predictor/firmware/hypo.c model ported to
// minimed_hypo.c. Fires at a decision point (minimed_hypo_should_evaluate, about 16 times a day on
// the wearer this was fitted on), or earlier still when minimed_hypo_falling_fast says the reading
// is about to plunge into that regime faster than it can wait for -- see its own comment for the
// physiological rate this is anchored on.
// The hypo model's own popup, like a pump alarm's: once when the treat score first reaches
// HYPO_ALERT_TREAT_PCT, then not again until it has dropped below (or left the falling-low regime)
// AND the cooldown has passed, so a score hovering at the threshold cannot buzz every 5 minutes.
// The threshold is the watchface's default TREAT level (HYPO_TREAT_THRESHOLD_DEFAULT there, the
// model's validated cut-off); the phone-set threshold only reaches the watchface. Gated on the
// same "low alerts" setting as the pump's own low alarms.
#define HYPO_ALERT_TREAT_PCT 32
#define HYPO_ALERT_COOLDOWN_SECS (30 * 60)
static bool s_hypo_alert_armed = true;
static uint32_t s_hypo_alert_last;

// mg/dL -> "N.N" mmol/L, rounded the way the pump displays it (see prv_parse_and_show).
static void prv_mmol_str(char *out, size_t n, float mgdl) {
  const int32_t m = (int32_t)(mgdl + 0.5f);
  const int32_t tenths = (m * 100000 + 90091) / 180182;
  snprintf(out, n, "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
}

static void prv_hypo_alert(const MinimedHypoPrediction *h, int32_t treat_pct, bool early) {
  const uint32_t now = (uint32_t)rtc_get_time();
  if (treat_pct < HYPO_ALERT_TREAT_PCT) {
    s_hypo_alert_armed = true;
    return;
  }
  if (!s_hypo_alert_armed ||
      (s_hypo_alert_last != 0 && now - s_hypo_alert_last < HYPO_ALERT_COOLDOWN_SECS) ||
      !minimed_settings_alert_enabled(true)) {
    return;
  }
  s_hypo_alert_armed = false;
  s_hypo_alert_last = now;
  char nadir[16], nadir_treated[16];
  prv_mmol_str(nadir, sizeof(nadir), h->nadir_untreated);
  prv_mmol_str(nadir_treated, sizeof(nadir_treated), h->nadir_treated);
  char body[160];
  snprintf(body, sizeof(body),
           "%sTreat %ld%%, low chance %d%% (severe %d%%)\n"
           "Lowest %s in the next hour, %s if treated\n"
           "~%ld min below 3.9\nNow %s",
           early ? "Falling fast. " : "", (long)treat_pct, (int)(h->p_low * 100.0f + 0.5f),
           (int)(h->p_severe * 100.0f + 0.5f), nadir, nadir_treated,
           (long)(h->mins_untreated + 0.5f), s_last_bg_str[0] != '\0' ? s_last_bg_str : "---");
  minimed_sake_log("hypo alert (model)");
  PBL_LOG_INFO("minimed: hypo alert shown: treat=%ld pct%s", (long)treat_pct,
               early ? " (early fast-fall)" : "");
  minimed_alert_popup_push("Low predicted (model)", body);
}

static void prv_hypo_check(const MinimedPredictWindow *win) {
  if (!minimed_settings_hypo_enabled()) {
    return;  // phone-disabled: skip the computation entirely, not just the send/display
  }
  const bool normal = win && minimed_hypo_should_evaluate(win);
  const bool early = win && !normal && minimed_hypo_falling_fast(win);
  if (!normal && !early) {
    minimed_sake_sender_send_hypo(false, 0, 0);  // out of the falling-low regime: clear any banner
    s_hypo_alert_armed = true;
    return;
  }
  MinimedHypoPrediction h;
  const RtcTicks hypo_start = rtc_get_ticks();
  minimed_hypo_eval(win, 0.3f, &h);
  const uint32_t hypo_ms =
      (uint32_t)(((rtc_get_ticks() - hypo_start) * 1000) / RTC_TICKS_HZ);
  PBL_LOG_INFO("minimed: hypo%s treat=%d pct, nadir %ld/%ld mg/dL (untreated/treated), mins<70 "
               "%ld/%ld, %lums",
               early ? " (early fast-fall)" : "", (int)(h.treat_pct + 0.5f),
               (long)(h.nadir_untreated + 0.5f), (long)(h.nadir_treated + 0.5f),
               (long)(h.mins_untreated + 0.5f), (long)(h.mins_treated + 0.5f),
               (unsigned long)hypo_ms);
  PBL_LOG_INFO("minimed: hypo p_low=%d pct p_severe=%d pct p_over=%d pct",
               (int)(h.p_low * 100.0f + 0.5f), (int)(h.p_severe * 100.0f + 0.5f),
               (int)(h.p_over * 100.0f + 0.5f));
  char line[64];
  snprintf(line, sizeof(line), "hypo%s t%d n%ld/%ld m%ld/%ld", early ? "!" : "",
           (int)(h.treat_pct + 0.5f), (long)(h.nadir_untreated + 0.5f),
           (long)(h.nadir_treated + 0.5f), (long)(h.mins_untreated + 0.5f),
           (long)(h.mins_treated + 0.5f));
  minimed_sake_log(line);
  const int32_t pct = (int32_t)(h.treat_pct + 0.5f);
  const int32_t p_low_pct = (int32_t)(h.p_low * 100.0f + 0.5f);
  prv_hypo_alert(&h, pct, early);
  minimed_sake_sender_send_hypo(true, (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct)),
                                (uint8_t)(p_low_pct < 0 ? 0 : (p_low_pct > 100 ? 100 : p_low_pct)));
}

// Predict 30 minutes ahead from the newest reading and hand the result to the watchface. A reading
// that cannot be predicted (stale, or the predictor not primed) clears the last prediction.
static void prv_predict_and_send(void) {
  MinimedPrediction p;
  const uint32_t now = (uint32_t)rtc_get_time();
  const RtcTicks predict_start = rtc_get_ticks();
  const bool have_pred = s_pred_ready && minimed_predict_run(&s_pred, now, prv_gmt_offset(now), &p);
  const uint32_t predict_ms =
      (uint32_t)(((rtc_get_ticks() - predict_start) * 1000) / RTC_TICKS_HZ);
  if (have_pred) {
    const int32_t pred = (int32_t)(p.mgdl + 0.5f);
    PBL_LOG_INFO("minimed: predict %ld mg/dL in 30 min (now %ld, %+ld), low p=%d/1000 alarm=%d, "
                 "%lums",
                 (long)pred, (long)s_reading_mgdl, (long)(pred - s_reading_mgdl),
                 (int)(p.low_prob * 1000.0f), (int)p.low_alarm, (unsigned long)predict_ms);
    PBL_LOG_INFO("minimed: predict inputs bg=%u/48 cells, slope15=%+d.%d mg/dL/min, ins4h=%d.%02dU, "
                 "carb4h=%dg, hour=%u",
                 p.bg_cells, p.slope_x10 / 10, (p.slope_x10 < 0 ? -p.slope_x10 : p.slope_x10) % 10,
                 p.ins_cu / 100, p.ins_cu % 100, p.carb_g, p.hour);
    // The watch's log view keeps 31 characters after the time; the rest is cut there.
    char line[64];
    snprintf(line, sizeof(line), "pred %ld %+ld low%d.%d%%", (long)pred,
             (long)(pred - s_reading_mgdl), (int)(p.low_prob * 100.0f),
             (int)(p.low_prob * 1000.0f) % 10);
    minimed_sake_log(line);
    snprintf(line, sizeof(line), "in bg%u s%+d.%d i%d.%dU c%dg h%u", p.bg_cells, p.slope_x10 / 10,
             (p.slope_x10 < 0 ? -p.slope_x10 : p.slope_x10) % 10, p.ins_cu / 100,
             (p.ins_cu % 100) / 10, p.carb_g, p.hour);
    minimed_sake_log(line);
    minimed_predict_score_note(&s_pred_score, now, pred, s_reading_mgdl);
    minimed_sake_sender_send_prediction(true, pred);
    prv_hypo_check(minimed_predict_last_window());
  } else {
    minimed_sake_sender_send_prediction(false, 0);
  }
}

// A live reading (not a backfilled one): score the forecast that came due, feed the predictor
// and refresh the prediction.
static void prv_predict_reading(int32_t mgdl) {
  int32_t err = 0;
  const bool scored = minimed_predict_score_actual(&s_pred_score, s_reading_ts, mgdl, &err);
  const uint32_t r = minimed_predict_score_rmse_x10(&s_pred_score, false);
  const uint32_t b = minimed_predict_score_rmse_x10(&s_pred_score, true);
  if (scored) {
    PBL_LOG_INFO("minimed: predict score n=%lu err=%+ld rmse=%lu.%lu carry-forward=%lu.%lu mg/dL "
                 "since start",
                 (unsigned long)s_pred_score.count, (long)err, (unsigned long)(r / 10),
                 (unsigned long)(r % 10), (unsigned long)(b / 10), (unsigned long)(b % 10));
  }
  // Always show the score in the watch's log view, also before the first forecast is due.
  char line[64];
  if (s_pred_score.count == 0) {
    const uint32_t first_due = s_pred_score.pending ? s_pred_score.due[0] : 0;
    const uint32_t wait_min =
        first_due > s_reading_ts ? (first_due - s_reading_ts + 59) / 60 : 0;
    snprintf(line, sizeof(line), "rmse n0, first in %lum", (unsigned long)wait_min);
  } else {
    snprintf(line, sizeof(line), "rmse%lu.%lu cf%lu.%lu n%lu err%+ld", (unsigned long)(r / 10),
             (unsigned long)(r % 10), (unsigned long)(b / 10), (unsigned long)(b % 10),
             (unsigned long)s_pred_score.count, scored ? (long)err : 0L);
  }
  minimed_sake_log(line);
  PBL_LOG_INFO("minimed: predict %s", line);
  minimed_predict_add_bg(&s_pred, s_reading_ts, mgdl);
  prv_predict_and_send();
}

// A live insulin, meal or basal record: stamped on arrival, like the meal shown on the watchface.
static void prv_predict_live_event(const MinimedHistEvent *ev) {
  const uint32_t now = (uint32_t)rtc_get_time();
  if (ev->kind == MinimedHistEventInsulin) {
    minimed_predict_add_insulin(&s_pred, now, ev->value);
  } else if (ev->kind == MinimedHistEventMicro) {
    minimed_predict_add_micro(&s_pred, now, ev->value);
  } else if (ev->kind == MinimedHistEventCarbs) {
    minimed_predict_add_carbs(&s_pred, now, ev->value);
  } else if (ev->kind == MinimedHistEventBasal) {
    minimed_predict_set_basal(&s_pred, now, ev->value);
  }
}

// A live history record that may carry insulin, carbs or a basal rate: the predictor's inputs, and
// for a meal what the watchface shows. Records here have no usable reference time (the read starts
// after the last one), so each is timed by when we learn of it -- seconds after it happened.
static void prv_live_record(uint8_t rec_len) {
  MinimedHistMeal meal;
  MinimedHistInsulin ins;
  MinimedHistEvent ev = {0};
  if (minimed_history_parse_meal(s_hist, rec_len, &meal)) {
    if (meal.grams == 0) return;  // the pump logs a Meal record for a bolus without carbs too
    PBL_LOG_INFO("minimed: meal %u g seq=%lu", (unsigned)meal.grams, (unsigned long)meal.seq);
    minimed_sake_sender_send_meal(meal.grams, (uint32_t)rtc_get_time());
    ev.kind = MinimedHistEventCarbs;
    ev.value = (float)meal.grams;
    prv_predict_live_event(&ev);
  } else if (minimed_history_parse_insulin(s_hist, rec_len, &ins)) {
    if (ins.kind == MinimedHistInsulinBasalRate) {
      ev.kind = MinimedHistEventBasal;
      ev.value = ins.by_algorithm ? 0.0f : ins.amount;
    } else {
      ev.kind = ins.kind == MinimedHistInsulinMicro ? MinimedHistEventMicro
                                                    : MinimedHistEventInsulin;
      ev.value = ins.amount;
    }
    prv_predict_live_event(&ev);
  }
}

// Note the off-scale side of the newest SG sample in the log. When it turns off-scale, the CGM
// record that just arrived as a "0 mg/dL" marker may have been skipped for want of a side: read it
// again so it is judged with the side known.
static void prv_set_hist_edge(int edge) {
  if (edge == s_hist_edge) return;
  s_hist_edge = (uint8_t)edge;
  PBL_LOG_INFO("minimed: SG history edge -> %d", edge);
  if (edge != 0) prv_request(PEND_CGM);
}

// A live history record that may be an SG sample.
static void prv_hist_sg_record(uint8_t rec_len) {
  MinimedHistSg sg;
  if (!minimed_history_parse_sg(s_hist, rec_len, &sg)) return;
  prv_set_hist_edge(minimed_history_sg_edge(sg.sg));
}

// The backfill exchange ended: place what it collected relative to the newest sample, hand the
// samples to the graph, and prime the predictor with the whole window.
static void prv_backfill_finish(void) {
  s_backfill_run = false;
  s_backfill_clock.have_ref = false;
  uint32_t newest = 0;
  for (uint8_t i = 0; i < s_backfill_n; i++) {
    if (s_backfill_secs[i] > newest) newest = s_backfill_secs[i];
  }
  // Pump clock minus watch clock. With a live reading, the newest sample is the reading already
  // shown; without one, the pump clock read for this backfill (see s_pump_clock_offset).
  const bool by_reading = s_have_offset;
  const int32_t offset =
      by_reading ? (int32_t)(newest - s_reading_ts) : (s_pump_clock_known ? s_pump_clock_offset : 0);
  const bool anchored = by_reading || s_pump_clock_known;
  // The window ends at the anchor: the reading, or the pump's "now".
  const uint32_t window_end = by_reading ? newest : (uint32_t)((int32_t)rtc_get_time() + offset);
  #define BF_TS(secs) ((uint32_t)((int32_t)(secs) - offset))
  const uint32_t window_secs = BACKFILL_WINDOW_MIN * 60u;

  uint32_t ts[MINIMED_BACKFILL_MAX_POINTS];
  int32_t mgdl[MINIMED_BACKFILL_MAX_POINTS];
  uint8_t kept = 0;
  for (uint8_t i = 0; i < s_backfill_n && anchored; i++) {
    if (s_backfill_secs[i] > window_end || window_end - s_backfill_secs[i] > window_secs) continue;
    ts[kept] = BF_TS(s_backfill_secs[i]);
    mgdl[kept] = s_backfill_mgdl[i];
    kept++;
  }
  // The anchor check: the newest logged sample should be the reading the watch already shows. If
  // the values differ, a newer sample landed mid-read (or the two are not the same sample) and
  // the backfilled trace may sit one step off.
  int32_t newest_mgdl = -1;
  int newest_edge = 0;
  for (uint8_t i = 0; i < s_backfill_n; i++) {
    if (s_backfill_secs[i] == newest) {
      newest_mgdl = s_backfill_mgdl[i];
      newest_edge = minimed_history_sg_edge(s_backfill_raw[i]);
    }
  }
  if (s_backfill_n > 0) prv_set_hist_edge(newest_edge);
  PBL_LOG_INFO("minimed: backfill %u of %u samples, %u events, newest sg=%ld cgm=%ld anchor=%s seq=%lu",
               (unsigned)kept, (unsigned)s_backfill_n, (unsigned)s_backfill_ne, (long)newest_mgdl,
               (long)s_reading_mgdl,
               !by_reading ? (anchored ? "pump-clock" : "NONE")
                           : (newest_mgdl == s_reading_mgdl ? "match" : "MISMATCH"),
               (unsigned long)s_annunc_seq);
  if (kept > 0) {
    minimed_sake_sender_backfill_graph(ts, mgdl, kept);
  }

  // The predictor starts over from the log, so nothing is counted twice, then takes back the
  // reading the watch already had.
  minimed_predict_reset(&s_pred);
  for (uint8_t i = 0; i < kept; i++) minimed_predict_add_bg(&s_pred, ts[i], mgdl[i]);
  for (uint8_t i = 0; i < s_backfill_ne && anchored; i++) {
    if (s_backfill_esecs[i] > window_end || window_end - s_backfill_esecs[i] > window_secs) {
      continue;
    }
    const uint32_t ets = BF_TS(s_backfill_esecs[i]);
    switch ((MinimedHistEventKind)s_backfill_ekind[i]) {
      case MinimedHistEventInsulin: minimed_predict_add_insulin(&s_pred, ets, s_backfill_evalue[i]); break;
      case MinimedHistEventMicro: minimed_predict_add_micro(&s_pred, ets, s_backfill_evalue[i]); break;
      case MinimedHistEventBasal: minimed_predict_set_basal(&s_pred, ets, s_backfill_evalue[i]); break;
      case MinimedHistEventCarbs:
        minimed_predict_add_carbs(&s_pred, ets, s_backfill_evalue[i]);
        // Every meal in the window, not just the newest -- the sender keeps all of them for the
        // watchface's meal list (KEY_MEAL_LIST) and the newest as the legacy single-meal fields.
        minimed_sake_sender_send_meal((uint16_t)(s_backfill_evalue[i] + 0.5f), ets);
        break;
      default: break;
    }
  }
  #undef BF_TS
  if (s_have_offset) minimed_predict_add_bg(&s_pred, s_reading_ts, s_reading_mgdl);
  s_pred_ready = kept > 0;
  prv_predict_and_send();
  s_backfill_n = 0;
  s_backfill_ne = 0;
}

// A new reading landed REFILL_GAP_MIN or more after the previous one: at least one CGM cycle was
// missed (a brief radio dropout, not a real outage -- STALE_MINUTES/annunciations cover the bigger
// case). Re-run the same backfill read used at connect time so the pump's own history fills the
// hole in the graph and the predictor's window, instead of the gap sitting there for good.
static void prv_refill_if_gap(uint32_t prev_ts, uint32_t new_ts) {
  if (!s_annunc_have || !s_have_offset || !HAVE(MinimedChrIddRacp) || !HAVE(MinimedChrIddHistory)) {
    return;
  }
  if (prev_ts == 0 || new_ts <= prev_ts) return;  // no prior reading yet, or a clock step back
  const uint32_t gap_min = (new_ts - prev_ts) / 60;
  if (gap_min < REFILL_GAP_MIN) return;
  if (s_last_refill_ts != 0 && new_ts - s_last_refill_ts < REFILL_COOLDOWN_SECS) return;
  s_last_refill_ts = new_ts;
  s_backfill_wanted = true;
  char line[32];
  snprintf(line, sizeof(line), "refill: gap %lu min", (unsigned long)gap_min);
  minimed_sake_log(line);
  PBL_LOG_INFO("minimed: %s, re-reading history", line);
  prv_request(PEND_ANNUNC);
}

// Queue the backfill read once both of its inputs exist: the log cursor (baseline) and something
// to anchor sample times on -- a CGM reading, or failing that the pump's clock, read first.
static void prv_backfill_maybe_request(void) {
  if (s_backfill_done || !s_annunc_have || !HAVE(MinimedChrIddRacp) ||
      !HAVE(MinimedChrIddHistory)) {
    return;
  }
  if (!s_have_offset && !s_pump_clock_known) {
    if (!s_pump_clock_requested && HAVE(MinimedChrCurrentTime)) {
      s_pump_clock_requested = true;
      MinimedGattStatus status;
      if (!minimed_transport_read(MinimedChrCurrentTime, TagPumpClockRead, &status)) {
        PBL_LOG_INFO("minimed: pump clock read rc=0x%04x", status.code);
      }
    }
    return;  // the read's result calls back in here; a later reading also will
  }
  s_backfill_done = true;
  s_backfill_wanted = true;
  prv_request(PEND_ANNUNC);
}

static void prv_pump_clock_read_done(const MinimedEvent *e) {
  uint32_t pump_secs = 0;
  if (!e->status.ok || !minimed_history_parse_current_time(e->data, e->len, &pump_secs)) {
    PBL_LOG_INFO("minimed: pump clock read failed err=0x%04x len=%u", e->status.code,
                 (unsigned)e->len);
    return;  // the backfill waits for a live reading instead
  }
  s_pump_clock_offset = (int32_t)(pump_secs - (uint32_t)rtc_get_time());
  s_pump_clock_known = true;
  PBL_LOG_INFO("minimed: pump clock %lu, offset %ld s: backfill anchored on it",
               (unsigned long)pump_secs, (long)s_pump_clock_offset);
  prv_backfill_maybe_request();
}

// One reassembled history record is complete: advance the cursor, and post a notification for a
// new, un-silenced annunciation raise (never during the baseline read).
static void prv_annunc_record_done(void) {
  MinimedAnnunciation a;
  const MinimedAnnuncRecord r = minimed_annunciation_parse_record(s_hist, s_hist_len, &a);
  const uint8_t rec_len = s_hist_len;
  s_hist_len = 0;
  if (r == MinimedAnnuncRecordBad) {
    PBL_LOG_INFO("minimed: bad hist rec len=%u %02x %02x %02x %02x", (unsigned)rec_len, s_hist[0],
                 s_hist[1], s_hist[2], s_hist[3]);
    return;
  }
  s_annunc_seen = true;
  if (a.seq > s_annunc_seq) s_annunc_seq = a.seq;
  if (r != MinimedAnnuncRecordYes) {
    // "Other" is a valid record of another event type, and SG measurements, NGP reference times
    // and meals are all "Other". Feed the backfill and meal parsers first: logging these and
    // returning here (2026-09-16) silently disabled both the graph backfill and meal forwarding.
    if (s_backfill_run) {
      prv_backfill_record(rec_len);
    } else if (!s_annunc_baseline) {
      prv_live_record(rec_len);
      prv_hist_sg_record(rec_len);
    }
    // Document unparsed event streams (the sensor-change burst) for #16 research. Skipped during a
    // backfill, which reads hundreds of SG records of its own.
    if (!s_backfill_run) {
      PBL_LOG_INFO("SAKE: hist type=0x%04x seq=%lu len=%u %02x%02x%02x",
                   (unsigned)(s_hist[0] | (s_hist[1] << 8)), (unsigned long)a.seq, rec_len,
                   s_hist[2], s_hist[3], s_hist[4]);
    }
    return;
  }

  // Every annunciation to flash, notified or not: this is also the field log that grows the
  // code/status catalog (docs/PUMP-DATA.md table).
  PBL_LOG_INFO("minimed: annunc type=0x%03x id=%u status=0x%02x sil=%d seq=%lu base=%d",
               (unsigned)a.type, (unsigned)a.id, (unsigned)a.status, (int)a.silenced,
               (unsigned long)a.seq, (int)s_annunc_baseline);
  if (s_annunc_baseline) return;
  if (a.silenced) return;  // the pump raised it quietly (alert settings); mirror that choice
  // Phone-configured (Settings page -> KEY_SETTINGS_ALERTS -> minimed_settings), not a compile
  // flag: see minimed_settings.h.
  if (!minimed_settings_alert_enabled(minimed_annunciation_is_low(a.type))) return;
  if (prv_annunc_already_notified(a.id)) return;

  char name[28];
  const char *known = minimed_annunciation_name(a.type);
  if (known != NULL) {
    snprintf(name, sizeof(name), "%s", known);
  } else {
    snprintf(name, sizeof(name), "Pump alert 0x%03x", (unsigned)a.type);
  }
  minimed_sake_log(name);
  // Body: for a predicted-low alert, the name with the latest BG in parens, e.g.
  // "Alert before low (4.2)" -- BG is at most one 5-min cycle old, and the CGM read dispatches
  // before this one on the same push, so on a fresh alert it is usually seconds old. Every other
  // alert just shows its name; the BG isn't relevant to e.g. a reservoir or battery alert.
  char body[48];
  if (s_last_bg_str[0] != '\0' && minimed_annunciation_shows_bg(a.type)) {
    snprintf(body, sizeof(body), "%s (%s)", name, s_last_bg_str);
  } else {
    snprintf(body, sizeof(body), "%s", name);
  }
  minimed_alert_popup_push("MiniMed", body);
}

// An inbound pump notification/indication on one of the characteristics the session uses.
static void prv_handle_notify(MinimedChr chr, const uint8_t *data, uint16_t len, bool truncated) {
  s_last_pump_traffic = (uint32_t)rtc_get_time();  // any pump notification = the link is alive
  if (truncated) {
    char line[32];
    snprintf(line, sizeof(line), "notify cut chr=%u", (unsigned)chr);
    minimed_sake_log(line);
    return;  // a cut SAKE frame fails its MAC anyway
  }
  if (chr == MinimedChrSensorExpiration) {
    // Sensor-info probe: the pump pushes Time Of Sensor Expiration here (indicate-only char).
    prv_sensorinfo_log_value("sensor exp", data, len);
    return;
  }
  if (chr == MinimedChrIddCommandCp || chr == MinimedChrIddCommandData) {
    prv_annunc_probe_notify(data, len);
    return;
  }
  if (chr == MinimedChrCgmMeasurement) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("CGM decrypt failed");
      return;
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
    return;
  }
  if (chr == MinimedChrIddStatusChanged) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("0x101 decrypt failed");
      PBL_LOG_INFO("minimed: 0x101 decrypt failed (%u bytes on the wire)", (unsigned)len);
      return;
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
    PBL_LOG_INFO("minimed: 0x101 push flags=0x%08x%08x (%u plaintext bytes)",
                 (unsigned)(flags >> 32), (unsigned)flags, (unsigned)plain_len);

    uint8_t req = 0;
    if (flags & MINIMED_IDD_FLAG_NEW_CGM) req |= PEND_CGM;
    if (flags & (MINIMED_IDD_FLAG_THERAPY_CONTROL | MINIMED_IDD_FLAG_OPERATIONAL |
                 MINIMED_IDD_FLAG_THERAPY_ALGORITHM)) {
      // Suspend/resume, an operational-state transition (reservoir-change walk: bit 1 is rare,
      // unlike bit 2 which rides every microbolus), or SmartGuard/temp-target changed: re-read
      // the status pair (bridge bits + bit 1).
      req |= (HAVE(MinimedChrIddStatus) ? PEND_STATUS : 0) | (HAVE(MinimedChrIddSrcp) ? PEND_TAS : 0);
    }
    if (HAVE(MinimedChrIddRacp) && HAVE(MinimedChrIddHistory) &&
        ((flags & (MINIMED_IDD_FLAG_ANNUNCIATION | MINIMED_IDD_FLAG_HISTORY_EVENT)) != 0 ||
         !s_annunc_have)) {
      // An alarm was raised or cleared, or any event was logged (a meal entry among them): read
      // the history records behind it. Until the baseline read has succeeded, any push doubles as
      // a retry of it.
      req |= PEND_ANNUNC;
    }
    if (HAVE(MinimedChrIddSrcp)) {
      if (flags & MINIMED_IDD_FLAG_IOB) req |= PEND_IOB;
      s_reset_flags |= flags;
      req |= PEND_RESET;  // no SRCP char would mean no reset possible; fallback still covers us
    }

    if (!s_push_mode) {
      s_push_mode = true;
      minimed_sake_log("push mode (6m fallback)");
    }
    // Re-arm the dead-man: an indication is proof push is alive.
    minimed_task_timer_start(TimerPoll, FALLBACK_AFTER_SECS * 1000);

    if (req != 0) prv_request(req);
    return;
  }
  if (chr == MinimedChrCgmRacp) {
    // Success is the common case and stays quiet so the log keeps scrolling BG readings; only an
    // unexpected RACP response is worth a line.
    bool ok = (len == sizeof(RACP_REPORT_SUCCESS) &&
               memcmp(data, RACP_REPORT_SUCCESS, len) == 0);
    if (!ok) {
      minimed_sake_log("RACP unexpected resp");
    }
    // Either way the CGM exchange is over. The serialiser then issues whatever is pending
    // (an IOB read queued with this poll, or a Reset Status). Ops don't strictly need
    // serialising for the stack's sake -- gattc ops queue FIFO -- but its 30 s unresponsive
    // timer starts at *queue* time, and the two reassembly buffers are single-exchange.
    if (s_op == PEND_CGM) {
      if (s_push_mode) {
        // A completed CGM exchange also proves the link; keep the dead-man from re-firing
        // right after a fallback-driven poll.
        minimed_task_timer_start(TimerPoll, FALLBACK_AFTER_SECS * 1000);
      }
      prv_op_complete();
    }
    return;
  }
  if (chr == MinimedChrIddHistory) {
    uint8_t plain[64];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("hist decrypt failed");
      return;
    }
    if (s_hist_len + plain_len > sizeof(s_hist)) {
      s_hist_len = 0;  // overflow guard; abandon this record
    }
    memcpy(s_hist + s_hist_len, plain, plain_len);
    s_hist_len += plain_len;
    // No length prefix: a wire fragment shorter than the ATT cap ends the record (the pump fills
    // notifications to the cap; the bridge's reassembler uses the same rule). An exact-multiple
    // record is flushed at the RACP terminal instead.
    if (len < minimed_transport_max_value_len()) prv_annunc_record_done();
    // A long catch-up read can outlive the 10 s op timer; each fragment is proof of progress.
    if (s_op == PEND_ANNUNC) {
      minimed_task_timer_start(TimerOpTimeout, OP_TIMEOUT_SECS * 1000);
    }
    return;
  }
  if (chr == MinimedChrIddRacp) {
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
      if (s_backfill_run) {
        prv_backfill_finish();
        // The backfill exchange stood in for any annunciation catch-up queued behind it.
        s_pending |= PEND_ANNUNC;
      } else if (s_annunc_baseline && s_annunc_seen) {
        s_annunc_have = true;
        PBL_LOG_INFO("minimed: annunc baseline seq=%lu", (unsigned long)s_annunc_seq);
        prv_backfill_maybe_request();
      }
      prv_op_complete();
    }
    return;
  }
  if (chr == MinimedChrIddSrcp) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("SRCP decrypt failed");
      return;
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
      return;
    }
    if (s_op == PEND_TAS) {
      // Single short indication (max ~14 plaintext bytes); no reassembly needed.
      if (!minimed_status_parse_tas(plain, plain_len, &s_tas)) {
        minimed_sake_log("TAS bad resp");
      } else {
        PBL_LOG_INFO("minimed: tas auto=%d shield=%02x ready=%02x tt=%u", (int)s_tas.has_auto_mode,
                     s_tas.shield, s_tas.readiness, (unsigned)s_tas.temp_target_min);
      }
      prv_status_publish_if_done(PEND_TAS);
      prv_op_complete();
      return;
    }
    if (s_op != PEND_IOB) {
      minimed_sake_log("SRCP unsolicited");
      return;
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
    return;
  }
}

// Layer 3 diagnostic: classify a GATT failure. A link-dead status (the transport's verdict: the
// stack knows the pump connection is gone, and the disconnect event should follow shortly) is
// told apart from an op-level error on a live link, so a silent pump drop is attributable.
// `issue` marks a failure to even start the operation rather than an error response.
static void prv_log_gatt_err(const char *op, MinimedGattStatus status, bool issue) {
  char line[40];
  if (status.link_dead) {
    snprintf(line, sizeof(line), "LINK DEAD %s %s=0x%04x", op, issue ? "rc" : "err", status.code);
  } else {
    snprintf(line, sizeof(line), "%s %s=0x%04x", op, issue ? "rc" : "err", status.code);
  }
  minimed_sake_log(line);
  if (status.link_dead && !issue) {
    PBL_LOG_WRN("minimed: %s failed with link-dead status 0x%04x", op, (unsigned)status.code);
  }
}

static void prv_op_complete(void) {
  minimed_task_timer_stop(TimerOpTimeout);
  s_op = 0;
  if (s_pending != 0) {
    minimed_task_timer_start(TimerDispatch, DISPATCH_DELAY_MS);
  }
  // Any completed exchange proves the pump responded to a request; record it as traffic.
  s_last_pump_traffic = (uint32_t)rtc_get_time();
  // The op is the unit of pump work: all of its state setters have run, so emit one frame.
  minimed_sake_sender_commit();
}

// Watchdog tick: if in DUAL with the pump connected but silent for PUMP_WD_NO_TRAFFIC_SECS,
// re-toggle DUAL to free the phantom slot and re-arm the pump advert.
static void prv_wd_timer(void) {
  if (minimed_sake_get_mode() == MinimedSakeModeDual && minimed_sake_pump_connected() &&
      ((uint32_t)rtc_get_time() - s_last_pump_traffic) > PUMP_WD_NO_TRAFFIC_SECS) {
    minimed_sake_log("WD: pump silent, re-toggle");
    PBL_LOG_WRN("minimed: pump silent %us, re-toggling DUAL",
                (unsigned)((uint32_t)rtc_get_time() - s_last_pump_traffic));
    minimed_sake_watchdog_retoggle();
  }
  minimed_task_timer_start(TimerWatchdog, 60 * 1000);
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
  if (minimed_status_compose(label, sizeof(label))) {
    const MinimedStatusTimers timers = minimed_status_get_timers();
    minimed_sake_sender_send_status(label, timers.start, timers.end);
    char line[32];
    snprintf(line, sizeof(line), "st: %s", label[0] != '\0' ? label : "(normal)");
    minimed_sake_log(line);
    // minimed_status_compose returns true on every status poll once primed (including the common
    // "(normal)" case), so this would otherwise log at INFO on every poll cycle forever.
    PBL_LOG_DBG("minimed: status label '%s' bg_invalid=%d", label,
                (int)minimed_status_bg_invalid());
    if (minimed_status_bg_invalid()) {
      // The pump has no valid glucose right now (warm-up, signal lost, ...): blank the BG
      // immediately, stamped now so the watchface shows a current "---" like the pump does,
      // instead of an old number with a climbing age. The next real reading overwrites it.
      minimed_sake_sender_send_bg("---", now);
      s_last_bg_str[0] = '\0';  // no valid glucose: alert notifications drop the BG body
    }
  }
  if (minimed_status_take_bg_became_valid()) {
    // Sensor recovered. Fetch a reading now rather than waiting for the pump to set its "new CGM"
    // push bit, which it need not do if the record it is displaying already existed -- that left
    // the watch blank for up to FALLBACK_AFTER_SECS while the pump showed a number.
    minimed_sake_log_evt("BG valid again -> fetch CGM");
    prv_request(PEND_CGM);
  }
  s_idd_st.valid = false;
  s_tas.valid = false;
}

// IDD Status (0x102) is a plain encrypted READ -- the one exchange that completes with its own
// read result rather than via an indication.
static void prv_idd_status_read_done(const MinimedEvent *e) {
  if (s_op != PEND_STATUS) return;  // late/stale result; a newer op owns the buffers now
  char line[32];
  if (e->status.ok && e->len > 0) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(e->data, e->len, plain, sizeof(plain), &plain_len)) {
      minimed_sake_log("st decrypt failed");
    } else if (!minimed_status_parse_idd(plain, plain_len, &s_idd_st)) {
      snprintf(line, sizeof(line), "st bad len=%u", plain_len);
      minimed_sake_log(line);
    } else {
      PBL_LOG_INFO("minimed: status t=%02x o=%02x fl=%02x conn=%02x msg=%02x res=%ld mu",
                   s_idd_st.therapy, s_idd_st.operational, s_idd_st.flags, s_idd_st.sensor_conn,
                   s_idd_st.sensor_msg, (long)s_idd_st.reservoir_mu);
    }
  } else {
    prv_log_gatt_err("st read", e->status, false);
  }
  prv_status_publish_if_done(PEND_STATUS);
  prv_op_complete();
}

// Queue work and kick the dispatcher. Callers guard on the characteristics they need (PEND_IOB
// and PEND_RESET require the SRCP), so the dispatcher never has to skip a queued op.
static void prv_request(uint8_t mask) {
  s_pending |= mask;
  if (s_op == 0) {
    minimed_task_timer_start(TimerDispatch, DISPATCH_DELAY_MS);
  }
}

// Encrypt and write an SRCP request (IOB, TAS, Reset Status). False: the op never started.
static bool prv_srcp_write(const uint8_t *plain, uint16_t plain_len, const char *what) {
  uint8_t enc[24];  // SeqCrypt appends a 1-byte counter + 2-byte MAC
  uint16_t enc_len = 0;
  if ((size_t)plain_len + 3 > sizeof(enc) || !minimed_sake_encrypt(plain, plain_len, enc, &enc_len)) {
    char line[32];
    snprintf(line, sizeof(line), "%s encrypt failed", what);
    minimed_sake_log(line);
    return false;
  }
  s_srcp_len = 0;
  MinimedGattStatus status;
  if (!minimed_transport_write(MinimedChrIddSrcp, enc, enc_len, TagSrcpWrite, &status)) {
    prv_log_gatt_err(what, status, true);
    return false;
  }
  return true;
}

// Issue the highest-priority pending exchange. Reads before reset (data lands ASAP; one reset
// then covers a whole indication burst). On a failed issue, complete immediately -- no
// indication will terminate an exchange that never started.
static void prv_dispatch_timer(void) {
  if (s_op != 0) return;  // in flight; prv_op_complete re-kicks
  MinimedGattStatus status;
  if (s_pending & PEND_CGM) {
    s_pending &= ~PEND_CGM;
    s_op = PEND_CGM;
    s_rec_len = 0;  // reassembly reset at issue time, not in a free-running poll
    if (!minimed_transport_write(MinimedChrCgmRacp, RACP_REPORT_LAST_RECORD,
                                 sizeof(RACP_REPORT_LAST_RECORD), TagCgmRacpWrite, &status)) {
      prv_log_gatt_err("RACP", status, true);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_ANNUNC) {
    s_pending &= ~PEND_ANNUNC;
    s_op = PEND_ANNUNC;
    s_hist_len = 0;
    s_backfill_run = s_backfill_wanted;
    s_backfill_wanted = false;
    s_backfill_n = 0;
    s_backfill_ne = 0;
    s_backfill_clock.have_ref = false;
    // Backfill reads history the pump already showed: suppress notifications like the baseline.
    s_annunc_baseline = !s_annunc_have || s_backfill_run;
    s_annunc_seen = false;
    // IDD RACP is plaintext. Baseline: report last record (33 69 0f) to learn the newest
    // sequence number. Catch-up: report within range (33 5a 0f + min/max u32 LE) from the
    // cursor; the open-ended max is a HW question -- the terminal response will say if the
    // pump insists on a real upper bound.
    uint8_t req[11] = {0x33, 0x69, 0x0F};
    uint16_t req_len = 3;
    if (s_annunc_have) {
      req[1] = 0x5A;
      // Backfill: the fixed span behind the newest known record. Catch-up: everything after it.
      uint32_t lo = s_annunc_seq + 1;
      uint32_t hi = 0xFFFFFFFF;
      if (s_backfill_run) {
        lo = (s_annunc_seq > BACKFILL_SEQ_SPAN) ? s_annunc_seq - BACKFILL_SEQ_SPAN : 1;
        hi = s_annunc_seq;
      }
      for (int i = 0; i < 4; i++) {
        req[3 + i] = (uint8_t)(lo >> (8 * i));
        req[7 + i] = (uint8_t)(hi >> (8 * i));
      }
      req_len = 11;
    }
    if (!minimed_transport_write(MinimedChrIddRacp, req, req_len, TagIddRacpWrite, &status)) {
      prv_log_gatt_err("IDD RACP", status, true);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_IOB) {
    s_pending &= ~PEND_IOB;
    s_op = PEND_IOB;
    if (!prv_srcp_write(SRCP_GET_IOB, sizeof(SRCP_GET_IOB), "IOB")) {
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_STATUS) {
    s_pending &= ~PEND_STATUS;
    s_op = PEND_STATUS;
    if (!minimed_transport_read(MinimedChrIddStatus, TagIddStatusRead, &status)) {
      prv_log_gatt_err("st", status, true);
      prv_status_publish_if_done(PEND_STATUS);
      prv_op_complete();
      return;
    }
  } else if (s_pending & PEND_TAS) {
    s_pending &= ~PEND_TAS;
    s_op = PEND_TAS;
    if (!prv_srcp_write(SRCP_GET_TAS, sizeof(SRCP_GET_TAS), "TAS")) {
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
    if (!prv_srcp_write(plain, sizeof(SRCP_RESET_STATUS) + flags_len, "rst")) {
      prv_op_complete();
      return;
    }
  } else {
    return;  // nothing pending
  }
  minimed_task_timer_start(TimerOpTimeout, OP_TIMEOUT_SECS * 1000);
}

// A lost terminating indication must not wedge the serialiser (fallback polls dispatch through
// it too, so a wedge would mean "no data", not "stale data"). Drop the exchange and move on.
static void prv_op_timeout_timer(void) {
  char line[32];
  snprintf(line, sizeof(line), "op timeout 0x%02x", s_op);
  minimed_sake_log(line);
  const uint8_t op = s_op;
  s_rec_len = 0;
  s_srcp_len = 0;
  s_hist_len = 0;
  s_op = 0;
  if (op == PEND_ANNUNC && s_backfill_run) {
    prv_backfill_finish();  // keep what arrived; the samples read so far are still good
  }
  if (op == PEND_STATUS || op == PEND_TAS) {
    // The timed-out read stays invalid; publish whatever the pair's other half delivered.
    prv_status_publish_if_done(op);
  }
  if (s_pending != 0) {
    minimed_task_timer_start(TimerDispatch, DISPATCH_DELAY_MS);
  }
  // Whatever the dropped op's partial work set (a backfill's meals and forecast) still goes out.
  minimed_sake_sender_commit();
}

// A write the session answers only on failure: its exchange ends with an indication, which no
// failed write will produce, so skip the exchange now rather than stalling until the op timeout.
static void prv_write_done(const MinimedEvent *e) {
  if (e->status.ok) return;
  if (e->tag == TagCgmRacpWrite) {
    prv_log_gatt_err("RACP wr", e->status, false);
    if (s_op == PEND_CGM) prv_op_complete();
  } else if (e->tag == TagIddRacpWrite) {
    prv_log_gatt_err("IDD RACP wr", e->status, false);
    if (s_op == PEND_ANNUNC) prv_op_complete();
  } else if (e->tag == TagSrcpWrite) {
    prv_log_gatt_err("SRCP wr", e->status, false);
    if (s_op == PEND_TAS) prv_status_publish_if_done(PEND_TAS);
    if (s_op == PEND_IOB || s_op == PEND_RESET || s_op == PEND_TAS) prv_op_complete();
  }
}

// Everything a full poll reads, gated on the characteristics this connection has.
static uint8_t prv_full_poll_mask(void) {
  return PEND_CGM | (HAVE(MinimedChrIddSrcp) ? (PEND_IOB | PEND_TAS) : 0) |
         (HAVE(MinimedChrIddStatus) ? PEND_STATUS : 0) |
         // Annunciation baseline rides the poll until it succeeds; after that only 0x101
         // annunciation pushes trigger reads.
         (HAVE(MinimedChrIddRacp) && HAVE(MinimedChrIddHistory) && !s_annunc_have ? PEND_ANNUNC
                                                                                  : 0);
}

// ---- Device Information, once per boot ----
// These only change across a pump firmware update or a pump swap, and the pump reconnects often
// enough that a per-session sweep would be log spam.

// v57 hard-faulted twice inside picolibc's %s conversion, with the fault correlating to this
// sweep. Unproven, but the plausible mechanism is stack exhaustion: v57 put 73 bytes of buffers
// in the callback's frame and then called PBL_LOG, which is itself stack-hungry. So the whole
// line is composed into one *static* buffer here and logged with a single %s. The sweep is
// serialised, so one shared buffer is safe.
static char s_devinfo_line[80];

static void prv_devinfo_read_next(void) {
  if (s_devinfo_idx >= DEVINFO_CHR_COUNT) {
    s_devinfo_read = true;  // whole sweep done; latch so it stays once per boot
    return;
  }
  const uint8_t idx = s_devinfo_idx;
  MinimedGattStatus status;
  if (!minimed_transport_read(s_devinfo_chrs[idx].chr, TagDevinfoRead, &status)) {
    // Abandon the sweep without latching, so the next session retries from the start.
    PBL_LOG_INFO("minimed: pump %s read rc=0x%04x", s_devinfo_chrs[idx].name, status.code);
    s_devinfo_idx = 0;
  }
}

static void prv_devinfo_read_done(const MinimedEvent *e) {
  const uint8_t idx = s_devinfo_idx;
  if (idx >= DEVINFO_CHR_COUNT || e->chr != s_devinfo_chrs[idx].chr) return;
  s_devinfo_idx = idx + 1;  // advance whatever happened: a field the pump won't give is skipped
  if (!e->status.ok || e->len < 1) {
    if (e->status.code != 0) {  // code 0: the pump just doesn't have it
      snprintf(s_devinfo_line, sizeof(s_devinfo_line), "%s read err=0x%04x",
               s_devinfo_chrs[idx].name, e->status.code);
      PBL_LOG_INFO("minimed: pump %s", s_devinfo_line);
    }
    prv_devinfo_read_next();
    return;
  }
  int off = snprintf(s_devinfo_line, sizeof(s_devinfo_line), "%s ", s_devinfo_chrs[idx].name);
  if (off >= 0 && (unsigned)off < sizeof(s_devinfo_line)) {
    if (s_devinfo_chrs[idx].hex) {
      for (uint16_t i = 0; i < e->len && (unsigned)off + 3 < sizeof(s_devinfo_line); i++) {
        off += snprintf(&s_devinfo_line[off], 3, "%02x", e->data[i]);
      }
    } else {
      unsigned room = sizeof(s_devinfo_line) - off - 1;
      uint16_t len = e->len > room ? (uint16_t)room : e->len;
      memcpy(&s_devinfo_line[off], e->data, len);
      while (len > 0 && s_devinfo_line[off + len - 1] == '\0') len--;  // trim a trailing NUL
      s_devinfo_line[off + len] = '\0';
    }
    PBL_LOG_INFO("minimed: pump %s", s_devinfo_line);
  }
  prv_devinfo_read_next();
}

static void prv_devinfo_timer(void) {
  if (s_devinfo_read) {
    return;
  }
  s_devinfo_idx = 0;
  prv_devinfo_read_next();
}

static void prv_battery_read_done(const MinimedEvent *e) {
  if (e->status.ok && e->len >= 1) {
    PBL_LOG_INFO("minimed: pump battery %u pct", (unsigned)e->data[0]);
  } else if (e->status.code != 0) {
    prv_log_gatt_err("battery read", e->status, false);
  }
}

// Kernel heap watch on a fixed interval, independent of the pump poll (which push mode keeps
// deferring). The OOM crash (kernel heap to ~2.7 KB, 2026-09-09) was only visible after the fact;
// report free/max-free every 30 min so a slow leak shows in the flash log before it kills the
// watch, without flooding the log.
#define HEAP_LOG_INTERVAL_SECS (30 * 60)
static void prv_heap_timer(void) {
  unsigned int used = 0, free_bytes = 0, max_free = 0;
  heap_calc_totals(kernel_heap_get(), &used, &free_bytes, &max_free);
  PBL_LOG_INFO("minimed: heap free=%u max_free=%u", free_bytes, max_free);
  minimed_task_timer_start(TimerHeap, HEAP_LOG_INTERVAL_SECS * 1000);
}

static void prv_battery_timer(void) {
  MinimedGattStatus status;
  if (!minimed_transport_read(MinimedChrBattery, TagBatteryRead, &status)) {
    PBL_LOG_INFO("minimed: pump battery read rc=0x%04x", status.code);
  }
  minimed_task_timer_start(TimerBattery, BATTERY_READ_INTERVAL_SECS * 1000);
}

// ---- Sensor-info probe ----

static const char *prv_sensorinfo_step_name(uint8_t step) {
  switch (step) {
    case SI_STEP_RUN_TIME: return "sess run";
    case SI_STEP_EXPIRATION: return "sensor exp";
    case SI_STEP_IDD_FEATURES: return "idd feat";
    case SI_STEP_SESSION_START: return "sess start";
    default: return "?";
  }
}

// Append hex; returns the new offset. Caller guarantees room.
static size_t prv_append_hex(char *dst, size_t cap, size_t off, const uint8_t *d, uint16_t n) {
  for (uint16_t i = 0; i < n && off + 3 < cap; i++) {
    off += (size_t)snprintf(&dst[off], 3, "%02x", d[i]);
  }
  dst[off] = '\0';
  return off;
}

// Log one probe value: raw hex on one line, then hex of a decrypt attempt on a second line.
// An encrypted characteristic shows up as a decrypt success; a plaintext one as a decrypt
// failure with sane raw bytes. Two short lines, not one: long flash-log records get split.
static void prv_sensorinfo_log_value(const char *name, const uint8_t *raw, uint16_t n) {
  size_t off = (size_t)snprintf(s_sensorinfo_line, sizeof(s_sensorinfo_line), "sensor: %s raw=", name);
  off = prv_append_hex(s_sensorinfo_line, sizeof(s_sensorinfo_line), off, raw, n);
  PBL_LOG_INFO("SAKE: %s", s_sensorinfo_line);
  uint8_t plain[24];
  uint16_t plain_len = 0;
  if (minimed_sake_decrypt(raw, n, plain, sizeof(plain), &plain_len)) {
    off = (size_t)snprintf(s_sensorinfo_line, sizeof(s_sensorinfo_line), "sensor: %s plain=", name);
    prv_append_hex(s_sensorinfo_line, sizeof(s_sensorinfo_line), off, plain, plain_len);
    PBL_LOG_INFO("SAKE: %s", s_sensorinfo_line);
  }
}

// Chained sweep. Steps 0 and 2 read by UUID over the whole handle range; step 1 is an
// indicate-only characteristic, so it subscribes instead of reading and the value logs from the
// notify handler when the pump pushes it; step 3 reads 0x2AAA by its discovered handle (a SIG
// UUID another service could also expose).
static void prv_sensorinfo_read(uint8_t step) {
  if (step >= SI_STEP_COUNT) {
    s_sensorinfo_done = true;
    minimed_sake_log("sensor probe done");
    return;
  }
  s_sensorinfo_step = step;
  MinimedGattStatus status;
  bool started;
  if (step == SI_STEP_SESSION_START) {
    if (!HAVE(MinimedChrCgmSessionStart)) {
      minimed_sake_log("sensor: no sess start chr");
      prv_sensorinfo_read(step + 1);
      return;
    }
    started = minimed_transport_read(MinimedChrCgmSessionStart, TagSessionStartRead, &status);
  } else if (step == SI_STEP_EXPIRATION) {
    if (!HAVE(MinimedChrSensorExpiration)) {
      minimed_sake_log("sensor: no sensor exp chr");
      prv_sensorinfo_read(step + 1);
      return;
    }
    started = minimed_transport_subscribe(MinimedChrSensorExpiration, true, TagSensorExpSub,
                                          &status);
  } else {
    const MinimedChr chr =
        (step == SI_STEP_IDD_FEATURES) ? MinimedChrIddFeatures : MinimedChrSessionRunTime;
    started = minimed_transport_read(chr, TagSensorInfoRead, &status);
  }
  if (!started) {
    snprintf(s_sensorinfo_line, sizeof(s_sensorinfo_line), "sensor: %s rc=0x%04x",
             prv_sensorinfo_step_name(step), status.code);
    minimed_sake_log(s_sensorinfo_line);
    prv_sensorinfo_read(step + 1);  // keep sweeping; failure of one step is not fatal
  }
}

static void prv_sensorinfo_done(const MinimedEvent *e) {
  const uint8_t step = s_sensorinfo_step;
  if (e->tag == TagSensorExpSub) {
    if (!e->status.ok) {
      snprintf(s_sensorinfo_line, sizeof(s_sensorinfo_line), "sensor: exp sub err=0x%04x",
               e->status.code);
      minimed_sake_log(s_sensorinfo_line);
    }
    prv_sensorinfo_read(SI_STEP_IDD_FEATURES);
    return;
  }
  if (e->status.ok && e->len >= 1) {
    prv_sensorinfo_log_value(prv_sensorinfo_step_name(step), e->data, e->len);
  } else if (e->status.code != 0) {
    snprintf(s_sensorinfo_line, sizeof(s_sensorinfo_line), "sensor: %s err=0x%04x",
             prv_sensorinfo_step_name(step), e->status.code);
    PBL_LOG_INFO("SAKE: %s", s_sensorinfo_line);
    if (e->tag == TagSessionStartRead) {
      prv_sensorinfo_read(SI_STEP_COUNT);
      return;
    }
  }
  prv_sensorinfo_read(step + 1);
}

static void prv_sensorinfo_timer(void) {
  if (s_sensorinfo_done) return;
  minimed_sake_log("sensor probe start");
  prv_sensorinfo_read(SI_STEP_RUN_TIME);
}

static void prv_poll_timer(void) {
  if (s_push_mode) minimed_sake_log("fallback poll");
  // Layer 4 diagnostic: does the stack still believe the pump link is connected? Runs on the same
  // cadence as the poll. If the pump has gone silent but the link is still there (and MTU is
  // sane), the silence is the pump not pushing, not a dropped link. If it is gone, the stack
  // knows even though no disconnect event has arrived yet.
  {
    uint16_t mtu = 0;
    char line[48];
    if (minimed_transport_link_alive(&mtu)) {
      snprintf(line, sizeof(line), "lnk mtu=%u", (unsigned)mtu);
    } else {
      snprintf(line, sizeof(line), "lnk GONE");
    }
    minimed_sake_log(line);
  }
  prv_request(prv_full_poll_mask());
  const uint32_t secs = s_push_mode ? FALLBACK_AFTER_SECS : POLL_INTERVAL_SECS;
  minimed_task_timer_start(TimerPoll, secs * 1000);
}

// ---- Snooze/Confirm Annunciation probe ----
// Does the pump implement the standard IDS Snooze Annunciation (0x0f69) and Confirm Annunciation
// (0x0f99) commands on the IDD Command Control Point? MiniMed Mobile never sends them, so nobody
// knows. Once per boot, send each with an instance ID no annunciation has (0xFFFF) and log the
// answer: "Procedure not applicable" (0x74) means the opcode is implemented and just found no such
// annunciation; "Opcode not supported" (0x70) means it is not. The bogus ID is what keeps this a
// pure probe: no real alarm is ever snoozed or dismissed. The current annunciation is read and
// logged first, for the instance ID a later real Confirm would use.
#define ANNUNC_PROBE_DELAY_SECS 45
#define ANNUNC_PROBE_TIMEOUT_SECS 10
#define ANNUNC_PROBE_INSTANCE 0xFFFF
#define IDD_CMD_RESPONSE_CODE 0x0F55
#define IDD_CMD_SNOOZE 0x0F69
#define IDD_CMD_SNOOZE_RESPONSE 0x0F96
#define IDD_CMD_CONFIRM 0x0F99
#define IDD_CMD_CONFIRM_RESPONSE 0x0FA5
static bool s_annunc_probe_done;       // once per boot, like the devinfo sweep
static uint16_t s_annunc_probe_wait;   // the command awaiting its answer; 0 = none
static char s_annunc_probe_line[96];

static const char *prv_idd_response_name(uint8_t code) {
  switch (code) {
    case 0x0F: return "success";
    case 0x70: return "opcode not supported";
    case 0x71: return "invalid operand";
    case 0x72: return "procedure not completed";
    case 0x73: return "parameter out of range";
    case 0x74: return "procedure not applicable (opcode implemented)";
    case 0x75: return "plausibility check failed";
    default: return "?";
  }
}

static const char *prv_probe_cmd_name(uint16_t op) {
  return op == IDD_CMD_SNOOZE ? "snooze" : "confirm";
}

static void prv_annunc_probe_finish(void) {
  s_annunc_probe_wait = 0;
  s_annunc_probe_done = true;
  minimed_task_timer_stop(TimerAnnuncProbe);
  minimed_sake_log("annunc probe done");
}

static void prv_annunc_probe_send(uint16_t op) {
  const uint8_t plain[] = {(uint8_t)op, (uint8_t)(op >> 8), (uint8_t)ANNUNC_PROBE_INSTANCE,
                           (uint8_t)(ANNUNC_PROBE_INSTANCE >> 8)};
  uint8_t enc[sizeof(plain) + 3];
  uint16_t enc_len = 0;
  MinimedGattStatus status = {0};
  if (!minimed_sake_encrypt(plain, sizeof(plain), enc, &enc_len) ||
      !minimed_transport_write(MinimedChrIddCommandCp, enc, enc_len, TagProbeWrite, &status)) {
    PBL_LOG_INFO("minimed: annunc probe %s: not sent (rc=0x%04x)", prv_probe_cmd_name(op),
                 status.code);
    if (op == IDD_CMD_SNOOZE) {
      prv_annunc_probe_send(IDD_CMD_CONFIRM);
    } else {
      prv_annunc_probe_finish();
    }
    return;
  }
  s_annunc_probe_wait = op;
  minimed_task_timer_start(TimerAnnuncProbe, ANNUNC_PROBE_TIMEOUT_SECS * 1000);
}

// The awaited command got its answer (or none): log it, then move on to the next one.
static void prv_annunc_probe_next(void) {
  const uint16_t done = s_annunc_probe_wait;
  s_annunc_probe_wait = 0;
  if (done == IDD_CMD_SNOOZE) {
    prv_annunc_probe_send(IDD_CMD_CONFIRM);
  } else {
    prv_annunc_probe_finish();
  }
}

static void prv_annunc_probe_notify(const uint8_t *data, uint16_t len) {
  uint8_t plain[24];
  uint16_t n = 0;
  if (!minimed_sake_decrypt(data, len, plain, sizeof(plain), &n)) {
    PBL_LOG_INFO("minimed: annunc probe: undecryptable command response (%u bytes)",
                 (unsigned)len);
    return;
  }
  size_t off = (size_t)snprintf(s_annunc_probe_line, sizeof(s_annunc_probe_line),
                                "annunc probe resp ");
  off = prv_append_hex(s_annunc_probe_line, sizeof(s_annunc_probe_line), off, plain, n);
  PBL_LOG_INFO("minimed: %s", s_annunc_probe_line);
  if (n < 2 || s_annunc_probe_wait == 0) return;
  const uint16_t op = (uint16_t)(plain[0] | (plain[1] << 8));
  if (op == IDD_CMD_RESPONSE_CODE && n >= 5) {
    const uint16_t req = (uint16_t)(plain[2] | (plain[3] << 8));
    if (req != s_annunc_probe_wait) return;
    PBL_LOG_INFO("minimed: annunc probe %s -> 0x%02x %s", prv_probe_cmd_name(req), plain[4],
                 prv_idd_response_name(plain[4]));
    char line[40];
    snprintf(line, sizeof(line), "%s -> 0x%02x", prv_probe_cmd_name(req), plain[4]);
    minimed_sake_log(line);
    prv_annunc_probe_next();
  } else if ((op == IDD_CMD_SNOOZE_RESPONSE && s_annunc_probe_wait == IDD_CMD_SNOOZE) ||
             (op == IDD_CMD_CONFIRM_RESPONSE && s_annunc_probe_wait == IDD_CMD_CONFIRM)) {
    // A success response for an ID that should not exist: note it, and still wait for the
    // Response Code indication that ends the command.
    PBL_LOG_INFO("minimed: annunc probe %s: success response (opcode implemented)",
                 prv_probe_cmd_name(s_annunc_probe_wait));
  }
}

static void prv_annunc_probe_timer(void) {
  if (s_annunc_probe_wait != 0) {
    PBL_LOG_INFO("minimed: annunc probe %s: no response in %u s",
                 prv_probe_cmd_name(s_annunc_probe_wait), ANNUNC_PROBE_TIMEOUT_SECS);
    prv_annunc_probe_next();
    return;
  }
  if (s_annunc_probe_done) return;
  if (!HAVE(MinimedChrIddCommandCp) || !HAVE(MinimedChrIddCommandData)) {
    PBL_LOG_INFO("minimed: annunc probe: no IDD Command CP/Data characteristic");
    s_annunc_probe_done = true;
    return;
  }
  minimed_sake_log("annunc probe start");
  MinimedGattStatus status;
  if (!minimed_transport_subscribe(MinimedChrIddCommandData, false, TagProbeSubData, &status)) {
    PBL_LOG_INFO("minimed: annunc probe: command data sub rc=0x%04x", status.code);
    s_annunc_probe_done = true;
  }
}

static void prv_annunc_probe_done(const MinimedEvent *e) {
  MinimedGattStatus status;
  switch (e->tag) {
    case TagProbeSubData:
      if (!e->status.ok ||
          !minimed_transport_subscribe(MinimedChrIddCommandCp, true, TagProbeSubCp, &status)) {
        PBL_LOG_INFO("minimed: annunc probe: subscribe failed err=0x%04x", e->status.code);
        s_annunc_probe_done = true;
      }
      return;
    case TagProbeSubCp:
      if (!e->status.ok) {
        PBL_LOG_INFO("minimed: annunc probe: command CP sub err=0x%04x", e->status.code);
        s_annunc_probe_done = true;
        return;
      }
      if (!HAVE(MinimedChrIddAnnuncStatus) ||
          !minimed_transport_read(MinimedChrIddAnnuncStatus, TagProbeStatusRead, &status)) {
        prv_annunc_probe_send(IDD_CMD_SNOOZE);
      }
      return;
    case TagProbeStatusRead: {
      uint8_t plain[24];
      uint16_t n = 0;
      if (e->status.ok && minimed_sake_decrypt(e->data, e->len, plain, sizeof(plain), &n) &&
          n >= 6) {
        // Flags(1) | instance ID(2) | type(2, low 12 bits) | status(1): IDD Annunciation Status.
        PBL_LOG_INFO("minimed: annunc status flags=%02x id=%u type=0x%03x status=0x%02x",
                     plain[0], (unsigned)(plain[1] | (plain[2] << 8)),
                     (unsigned)((plain[3] | (plain[4] << 8)) & 0x0fff), plain[5]);
      } else {
        PBL_LOG_INFO("minimed: annunc status read failed err=0x%04x len=%u", e->status.code,
                     (unsigned)e->len);
      }
      prv_annunc_probe_send(IDD_CMD_SNOOZE);
      return;
    }
    case TagProbeWrite:
      if (!e->status.ok && s_annunc_probe_wait != 0) {
        PBL_LOG_INFO("minimed: annunc probe %s: write err=0x%04x",
                     prv_probe_cmd_name(s_annunc_probe_wait), e->status.code);
        prv_annunc_probe_next();
      }
      return;
    default:
      return;
  }
}

// ---- Connection setup: subscriptions, then polling ----

// Begin the continuous CGM poll. IOB rides each poll only if the IDD SRCP char is usable; a
// missing/failed IDD discovery leaves BG working, just without IOB.
static void prv_start_polling(void) {
  minimed_sake_log(HAVE(MinimedChrIddSrcp) ? "polling BG + IOB" : "polling BG only");
  prv_request(prv_full_poll_mask());
  minimed_task_timer_start(TimerPoll, POLL_INTERVAL_SECS * 1000);
  minimed_task_timer_start(TimerBattery, BATTERY_FIRST_READ_DELAY_SECS * 1000);
  minimed_task_timer_start(TimerDevinfo, DEVINFO_READ_DELAY_SECS * 1000);
  minimed_task_timer_start(TimerSensorInfo, SENSORINFO_READ_DELAY_SECS * 1000);
  if (!s_annunc_probe_done) {
    minimed_task_timer_start(TimerAnnuncProbe, ANNUNC_PROBE_DELAY_SECS * 1000);
  }
  // Push subscription, deliberately LAST and deliberately fire-and-forget. Everything that
  // matters (BG, IOB) is already polling by this point, so a failure here -- or no indication
  // ever arriving -- just leaves the 60 s poll running; push mode only engages on the first
  // actual indication (see the 0x101 branch of prv_handle_notify).
  if (HAVE(MinimedChrIddStatusChanged)) {
    MinimedGattStatus status;
    minimed_transport_subscribe(MinimedChrIddStatusChanged, true, TagNone, &status);
    char line[32];
    snprintf(line, sizeof(line), "0x101 sub rc=%d", (int)status.code);
    minimed_sake_log(line);
    PBL_LOG_INFO("minimed: subscribed IDD Status Changed (0x101): rc=%d", (int)status.code);
  } else {
    PBL_LOG_INFO("minimed: no IDD Status Changed (0x101) characteristic found");
  }
}

// Annunciation subscriptions (IDD RACP indicate, then History Data notify), chained before
// polling starts. Any failure drops both -- no alerts, BG/IOB/status unaffected.
static void prv_annunc_give_up(const char *what, uint16_t code) {
  char line[32];
  snprintf(line, sizeof(line), "%s 0x%04x", what, code);
  minimed_sake_log(line);
  s_have[MinimedChrIddRacp] = false;
  s_have[MinimedChrIddHistory] = false;
  prv_start_polling();
}

static void prv_sub_annunc(void) {
  if (!HAVE(MinimedChrIddRacp) || !HAVE(MinimedChrIddHistory)) {
    s_have[MinimedChrIddRacp] = false;
    s_have[MinimedChrIddHistory] = false;
    minimed_sake_log("no IDD RACP/hist chr");
    prv_start_polling();
    return;
  }
  MinimedGattStatus status;
  if (!minimed_transport_subscribe(MinimedChrIddRacp, true, TagSubIddRacp, &status)) {
    prv_annunc_give_up("IDD RACP sub rc", status.code);
  }
}

// The IDD part of the setup, once the CGM characteristics are subscribed.
static void prv_setup_idd(void) {
  if (!HAVE(MinimedChrIddSrcp)) {
    minimed_sake_log("no IDD SRCP chr");
    prv_sub_annunc();  // BG still works without IOB
    return;
  }
  MinimedGattStatus status;
  if (!minimed_transport_subscribe(MinimedChrIddSrcp, true, TagSubSrcp, &status)) {
    char line[32];
    snprintf(line, sizeof(line), "SRCP sub rc=0x%04x", status.code);
    minimed_sake_log(line);
    s_have[MinimedChrIddSrcp] = false;
    prv_sub_annunc();
  }
}

// One step of the setup chain finished. Each step is chained off the previous one's response, so
// e.g. notifications are effective before the first RACP write that would produce them.
static void prv_setup_done(const MinimedEvent *e) {
  char line[40];
  MinimedGattStatus status;
  switch (e->tag) {
    case TagCgmFeatureRead: {
      if (!e->status.ok) {
        snprintf(line, sizeof(line), "feat read err=0x%04x", e->status.code);
        minimed_sake_log(line);
        return;
      }
      snprintf(line, sizeof(line), "CGM feat %u:%02x %02x", e->len, e->len > 0 ? e->data[0] : 0,
               e->len > 1 ? e->data[1] : 0);
      minimed_sake_log(line);
      if (!HAVE(MinimedChrCgmMeasurement) || !HAVE(MinimedChrCgmRacp)) {
        minimed_sake_log("missing meas/RACP chr");
        return;
      }
      if (!minimed_transport_subscribe(MinimedChrCgmMeasurement, false, TagSubMeasurement,
                                       &status)) {
        snprintf(line, sizeof(line), "meas sub rc=0x%04x", status.code);
        minimed_sake_log(line);
      }
      return;
    }
    case TagSubMeasurement:
      if (!e->status.ok) {
        snprintf(line, sizeof(line), "meas sub err=0x%04x", e->status.code);
        minimed_sake_log(line);
        return;
      }
      if (!minimed_transport_subscribe(MinimedChrCgmRacp, true, TagSubCgmRacp, &status)) {
        snprintf(line, sizeof(line), "RACP sub rc=0x%04x", status.code);
        minimed_sake_log(line);
      }
      return;
    case TagSubCgmRacp:
      if (!e->status.ok) {
        snprintf(line, sizeof(line), "RACP sub err=0x%04x", e->status.code);
        minimed_sake_log(line);
        return;
      }
      prv_setup_idd();
      return;
    case TagSubSrcp:
      if (!e->status.ok) {
        snprintf(line, sizeof(line), "SRCP sub err=0x%04x", e->status.code);
        minimed_sake_log(line);
        s_have[MinimedChrIddSrcp] = false;  // give up on IOB, keep BG
      }
      prv_sub_annunc();
      return;
    case TagSubIddRacp:
      if (!e->status.ok) {
        prv_annunc_give_up("IDD RACP sub err", e->status.code);
        return;
      }
      if (!minimed_transport_subscribe(MinimedChrIddHistory, false, TagSubHistory, &status)) {
        prv_annunc_give_up("hist sub rc", status.code);
      }
      return;
    case TagSubHistory:
      if (!e->status.ok) {
        prv_annunc_give_up("hist sub err", e->status.code);
        return;
      }
      prv_start_polling();
      return;
    default:
      return;
  }
}

// Discovery finished: note what this pump exposes, then start the setup chain with the CGM
// Feature read.
static void prv_discovered(void) {
  for (unsigned i = 0; i < MinimedChrCount; i++) {
    s_have[i] = minimed_transport_has((MinimedChr)i);
  }
  char line[40];
  snprintf(line, sizeof(line), "chrs: m=%d f=%d r=%d ss=%d i=%d", HAVE(MinimedChrCgmMeasurement),
           HAVE(MinimedChrCgmFeature), HAVE(MinimedChrCgmRacp), HAVE(MinimedChrCgmSessionStart),
           HAVE(MinimedChrIddSrcp));
  minimed_sake_log(line);
  if (!HAVE(MinimedChrCgmFeature)) {
    minimed_sake_log("no CGM feature chr!");
    return;
  }
  MinimedGattStatus status;
  if (!minimed_transport_read(MinimedChrCgmFeature, TagCgmFeatureRead, &status)) {
    snprintf(line, sizeof(line), "feat read rc=0x%04x", status.code);
    minimed_sake_log(line);
  }
}

// ---- Link lifecycle ----

static void prv_link_up(void) {
  minimed_task_timer_stop(TimerPoll);
  minimed_task_timer_stop(TimerDispatch);
  minimed_task_timer_stop(TimerOpTimeout);
  s_link_up = true;
  s_last_pump_traffic = (uint32_t)rtc_get_time();  // fresh baseline; pump just connected
  minimed_task_timer_start(TimerWatchdog, 60 * 1000);
  minimed_task_timer_start(TimerHeap, HEAP_LOG_INTERVAL_SECS * 1000);
  memset(s_have, 0, sizeof(s_have));  // re-discovered per connection; a stale one could alias
  s_sensorinfo_done = false;
  s_annunc_probe_wait = 0;  // a probe command cut off by the disconnect gets no answer now
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
  s_backfill_done = false;
  s_backfill_wanted = false;
  s_backfill_run = false;
  s_last_refill_ts = 0;
  s_backfill_clock.have_ref = false;
  s_backfill_n = 0;
  s_backfill_ne = 0;
  s_pump_clock_known = false;  // re-read per connection, like everything else here
  s_pump_clock_requested = false;
  s_pred_ready = false;  // primed again by this connection's backfill
  s_hist_edge = 0;
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
  // A beat after the handshake, so discovery doesn't start inside the SAKE-port write exchange
  // that completed it.
  minimed_task_timer_start(TimerKickoff, 250);
}

static void prv_link_down(void) {
  s_link_up = false;
  for (unsigned t = 0; t < TimerCount; t++) {
    minimed_task_timer_stop(t);
  }
}

static void prv_timer(uint8_t timer) {
  if (!s_link_up) return;  // a timer that fired just before the link went down
  switch (timer) {
    case TimerKickoff:
      minimed_sake_log("discovering pump svcs...");
      minimed_transport_discover();
      break;
    case TimerPoll: prv_poll_timer(); break;
    case TimerWatchdog: prv_wd_timer(); break;
    case TimerDispatch: prv_dispatch_timer(); break;
    case TimerOpTimeout: prv_op_timeout_timer(); break;
    case TimerBattery: prv_battery_timer(); break;
    case TimerHeap: prv_heap_timer(); break;
    case TimerDevinfo: prv_devinfo_timer(); break;
    case TimerSensorInfo: prv_sensorinfo_timer(); break;
    case TimerAnnuncProbe: prv_annunc_probe_timer(); break;
    default: break;
  }
}

static void prv_gatt_done(const MinimedEvent *e) {
  switch (e->tag) {
    case TagCgmRacpWrite:
    case TagIddRacpWrite:
    case TagSrcpWrite:
      prv_write_done(e);
      break;
    case TagIddStatusRead: prv_idd_status_read_done(e); break;
    case TagDevinfoRead: prv_devinfo_read_done(e); break;
    case TagBatteryRead: prv_battery_read_done(e); break;
    case TagSensorInfoRead:
    case TagSensorExpSub:
    case TagSessionStartRead:
      prv_sensorinfo_done(e);
      break;
    case TagProbeSubData:
    case TagProbeSubCp:
    case TagProbeStatusRead:
    case TagProbeWrite:
      prv_annunc_probe_done(e);
      break;
    case TagPumpClockRead: prv_pump_clock_read_done(e); break;
    default: prv_setup_done(e); break;
  }
}

void minimed_session_init(void) { minimed_settings_init(); }

void minimed_session_handle_event(const MinimedEvent *e) {
  switch ((MinimedEventType)e->type) {
    case MinimedEventLinkUp: prv_link_up(); return;
    case MinimedEventLinkDown: prv_link_down(); return;
    case MinimedEventTimer: prv_timer(e->tag); return;
    default: break;
  }
  if (!s_link_up) return;  // a result from a link that is already gone
  switch ((MinimedEventType)e->type) {
    case MinimedEventDiscovered: prv_discovered(); break;
    case MinimedEventNotify: prv_handle_notify((MinimedChr)e->chr, e->data, e->len, e->truncated); break;
    case MinimedEventGattDone: prv_gatt_done(e); break;
    default: break;
  }
}
