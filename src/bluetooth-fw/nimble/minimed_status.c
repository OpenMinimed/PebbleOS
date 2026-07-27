/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_status.h"

#include <stdio.h>
#include <string.h>

#include "minimed_iob.h"  // medfloat32 decode (reservoir)

// Guardian 4 warm-up is a fixed ~2 h (bridge measurement 2026-07-20: WARM_UP -> first reading in
// 1h59m), self-timed because the pump rejects "Get Sensor Warm-up Time Remaining" (0x0403). The
// countdown is cleared by the first real reading anyway, so a small over-estimate is benign.
#define WARMUP_DURATION_SECS (2 * 60 * 60)

// Sensor Connectivity State bit 2: no transmitter signal (out of range / in charger).
#define SENSOR_CONN_GST_SIGNAL_LOST 0x04

// SensorMessageState raw values (only the ones the mapping needs by name).
#define SENSOR_MSG_WAIT_TO_CALIBRATE 0x01
#define SENSOR_MSG_DO_NOT_CALIBRATE 0x02
#define SENSOR_MSG_CALIBRATION_REQUIRED 0x03
#define SENSOR_MSG_CALIBRATING 0x04
#define SENSOR_MSG_SEARCHING 0x05
#define SENSOR_MSG_NO_SIGNAL 0x06
#define SENSOR_MSG_CHANGE_SENSOR 0x07
#define SENSOR_MSG_WARM_UP 0x08
#define SENSOR_MSG_SG_BELOW 0x09
#define SENSOR_MSG_SG_ABOVE 0x0A
#define SENSOR_MSG_GST_BATTERY 0x0B
#define SENSOR_MSG_WAITING_WARM_UP 0x0D
#define SENSOR_MSG_NO_PAIRED_SENSOR 0x0E

// TAS response flag bits (IddStatus.kt / tas_flags.py).
#define TAS_RESPONSE_OPCODE 0x03FE
#define TAS_FLAG_AUTO_MODE (1 << 0)
#define TAS_FLAG_LGS (1 << 1)
#define TAS_FLAG_PLGM (1 << 2)
#define TAS_FLAG_TEMP_TARGET (1 << 3)
#define TAS_FLAG_WAIT_TO_CALIBRATE (1 << 4)
#define TAS_FLAG_SAFE_BASAL (1 << 5)

// The label ids, in no particular order; the priority lives in prv_map(). LABEL_UNSET means
// update() has never run with valid data.
typedef enum {
  LABEL_UNSET,
  LABEL_NORMAL,  // composes to "" -- the watchface hides the band
  LABEL_LOAD_RESERVOIR,
  LABEL_SUSPENDED,
  LABEL_SG_LOW,
  LABEL_SG_HIGH,
  LABEL_WARMUP,
  LABEL_SENSOR_UPDATING,
  LABEL_SEARCHING,
  LABEL_NO_SIGNAL,
  LABEL_CHANGE_SENSOR,
  LABEL_NO_SENSOR,
  LABEL_SENSOR_BATT,
  LABEL_CALIBRATE,
  LABEL_CALIBRATING,
  LABEL_BG_REQUIRED,
  LABEL_SMARTGUARD_OFF,
  LABEL_SAFE_BASAL,
  LABEL_TEMP_TARGET,
} MinimedStatusLabel;

static MinimedStatusLabel s_label = LABEL_UNSET;
static uint32_t s_warmup_expiry;   // 0 = not timing
static uint32_t s_suspend_since;   // 0 = not suspended
static uint32_t s_tt_expiry;       // 0 = no temp target
static bool s_bg_invalid;

bool minimed_status_parse_idd(const uint8_t *body, uint16_t len, MinimedIddStatus *out) {
  memset(out, 0, sizeof(*out));
  if (len != 9) return false;
  uint32_t res_raw = (uint32_t)body[2] | ((uint32_t)body[3] << 8) | ((uint32_t)body[4] << 16) |
                     ((uint32_t)body[5] << 24);
  int32_t res_mu = 0;
  if (!minimed_iob_decode_medfloat32_mu(res_raw, &res_mu)) return false;
  out->therapy = body[0];
  out->operational = body[1];
  out->reservoir_mu = res_mu;
  // body[6] is the flags byte (bit 0 = reservoir attached) -- not needed by the mapping.
  out->sensor_conn = body[7];
  out->sensor_msg = body[8];
  out->valid = true;
  return true;
}

bool minimed_status_parse_tas(const uint8_t *body, uint16_t len, MinimedTas *out) {
  memset(out, 0, sizeof(*out));
  if (len < 4) return false;
  const uint16_t opcode = (uint16_t)(body[0] | (body[1] << 8));
  if (opcode != TAS_RESPONSE_OPCODE) return false;
  const uint16_t flags = (uint16_t)(body[2] | (body[3] << 8));
  // Flag-gated fields, consumed in tas.py's exact order: auto-mode (shield+readiness), PLGM,
  // LGS, temp target, wait-to-calibrate, safe basal. Trailing bytes are tolerated.
  uint16_t off = 4;
  if (flags & TAS_FLAG_AUTO_MODE) {
    if (off + 2 > len) return false;
    out->has_auto_mode = true;
    out->shield = body[off++];
    out->readiness = body[off++];
  }
  if (flags & TAS_FLAG_PLGM) {
    if (off + 1 > len) return false;
    off += 1;
  }
  if (flags & TAS_FLAG_LGS) {
    if (off + 1 > len) return false;
    off += 1;
  }
  if (flags & TAS_FLAG_TEMP_TARGET) {
    if (off + 2 > len) return false;
    out->temp_target_min = (uint16_t)(body[off] | (body[off + 1] << 8));
    off += 2;
  }
  // wait-to-calibrate / safe-basal fields: nothing the mapping needs; bounds not enforced since
  // we never read past them.
  out->valid = true;
  return true;
}

// The bridge's priority chain (readStatus), most actionable first. Clauses whose input read
// failed simply don't fire.
static MinimedStatusLabel prv_map(const MinimedIddStatus *st, const MinimedTas *tas) {
  const bool stopped = st->valid && (st->therapy == MINIMED_THERAPY_STOP ||
                                     st->therapy == MINIMED_THERAPY_PAUSE);
  if (stopped && (st->operational == MINIMED_OP_PREPARING ||
                  st->operational == MINIMED_OP_PRIMING ||
                  st->operational == MINIMED_OP_WAITING)) {
    return LABEL_LOAD_RESERVOIR;  // delivery stopped *for a set change*, not a plain suspend
  }
  if (stopped) return LABEL_SUSPENDED;
  if (st->valid) {
    switch (st->sensor_msg) {
      case SENSOR_MSG_SG_BELOW: return LABEL_SG_LOW;    // off-scale: a "---" that's a severe low
      case SENSOR_MSG_SG_ABOVE: return LABEL_SG_HIGH;
      case SENSOR_MSG_WARM_UP:
      case SENSOR_MSG_WAITING_WARM_UP:
      case SENSOR_MSG_WAIT_TO_CALIBRATE: return LABEL_WARMUP;
      case SENSOR_MSG_DO_NOT_CALIBRATE: return LABEL_SENSOR_UPDATING;
      case SENSOR_MSG_SEARCHING: return LABEL_SEARCHING;
      case SENSOR_MSG_NO_SIGNAL: return LABEL_NO_SIGNAL;
      case SENSOR_MSG_CHANGE_SENSOR: return LABEL_CHANGE_SENSOR;
      case SENSOR_MSG_NO_PAIRED_SENSOR: return LABEL_NO_SENSOR;
      case SENSOR_MSG_GST_BATTERY: return LABEL_SENSOR_BATT;
      default: break;
    }
  }
  const bool tas_auto = tas->valid && tas->has_auto_mode;
  if ((st->valid && st->sensor_msg == SENSOR_MSG_CALIBRATION_REQUIRED) ||
      (tas_auto && tas->readiness == MINIMED_READINESS_CALIBRATION_REQUIRED)) {
    return LABEL_CALIBRATE;
  }
  if (st->valid && st->sensor_msg == SENSOR_MSG_CALIBRATING) return LABEL_CALIBRATING;
  // BG required outranks the loop state: visible even while SmartGuard is off, since the BG is
  // what re-enters it (the pump shows it for BG_RECOMMENDED too).
  if (tas_auto && (tas->readiness == MINIMED_READINESS_BG_REQUIRED ||
                   tas->readiness == MINIMED_READINESS_BG_RECOMMENDED)) {
    return LABEL_BG_REQUIRED;
  }
  if (tas_auto && tas->shield == MINIMED_SHIELD_OPEN_LOOP) return LABEL_SMARTGUARD_OFF;
  if (tas_auto && tas->shield == MINIMED_SHIELD_SAFE_BASAL) return LABEL_SAFE_BASAL;
  if (tas->valid && tas->temp_target_min > 0) return LABEL_TEMP_TARGET;
  return LABEL_NORMAL;
}

void minimed_status_update(const MinimedIddStatus *st, const MinimedTas *tas, uint32_t now) {
  if (!st->valid && !tas->valid) return;  // both reads failed: keep the last known state
  s_label = prv_map(st, tas);

  // Warm-up: self-timed, stamped on ENTRY only so re-reads don't keep restarting the clock.
  if (s_label == LABEL_WARMUP) {
    if (s_warmup_expiry == 0) s_warmup_expiry = now + WARMUP_DURATION_SECS;
  } else {
    s_warmup_expiry = 0;
  }
  // Suspend: count-up from entry.
  if (s_label == LABEL_SUSPENDED) {
    if (s_suspend_since == 0) s_suspend_since = now;
  } else {
    s_suspend_since = 0;
  }
  // Temp target: the pump reports live remaining minutes, so restamp on every read.
  s_tt_expiry = (s_label == LABEL_TEMP_TARGET) ? now + (uint32_t)tas->temp_target_min * 60 : 0;

  // Whether the pump has no valid glucose right now (why BG shows "---"). Judged only when the
  // IDD read succeeded; otherwise the previous verdict stands.
  if (st->valid) {
    const bool gst_lost = (st->sensor_conn & SENSOR_CONN_GST_SIGNAL_LOST) != 0;
    const uint8_t m = st->sensor_msg;
    const bool no_glucose_state =
        m == SENSOR_MSG_WARM_UP || m == SENSOR_MSG_WAITING_WARM_UP ||
        m == SENSOR_MSG_WAIT_TO_CALIBRATE || m == SENSOR_MSG_SEARCHING ||
        m == SENSOR_MSG_NO_SIGNAL || m == SENSOR_MSG_CHANGE_SENSOR ||
        m == SENSOR_MSG_NO_PAIRED_SENSOR || m == SENSOR_MSG_GST_BATTERY;
    s_bg_invalid = gst_lost || no_glucose_state;
  }
}

// Minutes -> "H:MM" (e.g. 9 -> "0:09", 130 -> "2:10").
static void prv_hmm(uint32_t minutes, char *out, uint16_t cap) {
  snprintf(out, cap, "%lu:%02lu", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
}

bool minimed_status_compose(uint32_t now, char *out, uint16_t cap) {
  if (s_label == LABEL_UNSET) return false;
  char hmm[12];
  switch (s_label) {
    case LABEL_WARMUP:
      if (s_warmup_expiry == 0) { snprintf(out, cap, "SENSOR WARM-UP"); break; }
      prv_hmm(s_warmup_expiry > now ? (s_warmup_expiry - now) / 60 : 0, hmm, sizeof(hmm));
      snprintf(out, cap, "WARM-UP %s", hmm);  // shorter than the plain label; clearly a countdown
      break;
    case LABEL_TEMP_TARGET:
      if (s_tt_expiry == 0) { snprintf(out, cap, "TEMP TARGET"); break; }
      prv_hmm(s_tt_expiry > now ? (s_tt_expiry - now) / 60 : 0, hmm, sizeof(hmm));
      snprintf(out, cap, "TEMP TARGET %s", hmm);
      break;
    case LABEL_SUSPENDED:
      if (s_suspend_since == 0) { snprintf(out, cap, "SUSPENDED"); break; }
      prv_hmm(now > s_suspend_since ? (now - s_suspend_since) / 60 : 0, hmm, sizeof(hmm));
      snprintf(out, cap, "SUSPENDED %s", hmm);
      break;
    case LABEL_NORMAL: out[0] = '\0'; break;
    case LABEL_LOAD_RESERVOIR: snprintf(out, cap, "LOAD RESERVOIR"); break;
    case LABEL_SG_LOW: snprintf(out, cap, "LOW"); break;
    case LABEL_SG_HIGH: snprintf(out, cap, "HIGH"); break;
    case LABEL_SENSOR_UPDATING: snprintf(out, cap, "SENSOR UPDATING"); break;
    case LABEL_SEARCHING: snprintf(out, cap, "SEARCHING"); break;
    case LABEL_NO_SIGNAL: snprintf(out, cap, "NO SIGNAL"); break;
    case LABEL_CHANGE_SENSOR: snprintf(out, cap, "CHANGE SENSOR"); break;
    case LABEL_NO_SENSOR: snprintf(out, cap, "NO SENSOR"); break;
    case LABEL_SENSOR_BATT: snprintf(out, cap, "SENSOR BATT"); break;
    case LABEL_CALIBRATE: snprintf(out, cap, "CALIBRATE"); break;
    case LABEL_CALIBRATING: snprintf(out, cap, "CALIBRATING"); break;
    case LABEL_BG_REQUIRED: snprintf(out, cap, "BG REQUIRED"); break;
    case LABEL_SMARTGUARD_OFF: snprintf(out, cap, "SMARTGUARD OFF"); break;
    case LABEL_SAFE_BASAL: snprintf(out, cap, "SAFE BASAL"); break;
    default: out[0] = '\0'; break;
  }
  return true;
}

bool minimed_status_ticking(void) {
  return s_warmup_expiry != 0 || s_suspend_since != 0 || s_tt_expiry != 0;
}

bool minimed_status_bg_invalid(void) { return s_bg_invalid; }

void minimed_status_reset(void) {
  s_label = LABEL_UNSET;
  s_warmup_expiry = 0;
  s_suspend_since = 0;
  s_tt_expiry = 0;
  s_bg_invalid = false;
}
