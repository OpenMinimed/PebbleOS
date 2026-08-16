/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Pure pump-status logic for the on-watch spike: parse the IDD Status characteristic (0x102)
//! and the Therapy Algorithm States SRCP response (0x03FD -> 0x03FE), map them to the single
//! status label the watchface shows (key 15), and compose the time-augmented display string
//! (warm-up countdown, temp-target countdown, suspend count-up). Ported from the bridge's
//! iterated implementation (BridgeForegroundService.readStatus/statusForWatch + StatusLabels.kt,
//! IddStatus.kt) so both displays behave identically. NimBLE-free -- host-tested in
//! tools/minimed_sake_hosttest.

//! Raw wire values (IddStatus.kt / OpenMinimed pump_status.py, tas_flags.py).
#define MINIMED_THERAPY_STOP 0x33
#define MINIMED_THERAPY_PAUSE 0x3C
#define MINIMED_THERAPY_RUN 0x55
#define MINIMED_OP_PREPARING 0x55
#define MINIMED_OP_PRIMING 0x5A
#define MINIMED_OP_WAITING 0x66
#define MINIMED_OP_READY 0x96
#define MINIMED_SHIELD_OPEN_LOOP 0x01
#define MINIMED_SHIELD_AUTO_BASAL 0x02
#define MINIMED_SHIELD_SAFE_BASAL 0x03
#define MINIMED_READINESS_BG_REQUIRED 1
#define MINIMED_READINESS_CALIBRATION_REQUIRED 4
#define MINIMED_READINESS_BG_RECOMMENDED 5

//! Decoded IDD Status (0x102) body. `valid` false = the read failed; the mapping then simply
//! skips the therapy/sensor clauses (mirrors the bridge treating a null read).
typedef struct {
  bool valid;
  uint8_t therapy;      // TherapyControlState raw
  uint8_t operational;  // OperationalState raw
  uint8_t sensor_conn;  // Sensor Connectivity State flag byte
  uint8_t sensor_msg;   // SensorMessageState raw
  int32_t reservoir_mu; // reservoir remaining, milli-IU (logging only; not displayed)
} MinimedIddStatus;

//! Decoded Therapy Algorithm States (0x03FE) response.
typedef struct {
  bool valid;
  bool has_auto_mode;       // shield/readiness fields were present
  uint8_t shield;           // AutoModeShieldState raw
  uint8_t readiness;        // AutoModeReadinessState raw
  uint16_t temp_target_min; // remaining temp-target minutes; 0 = not active/absent
} MinimedTas;

//! Parse a decrypted 9-byte IDD Status body (therapy, operational, reservoir medfloat32, flags,
//! sensor connectivity, sensor message; no E2E on the 780G). Returns false (and leaves *out
//! invalid) on a length mismatch.
bool minimed_status_parse_idd(const uint8_t *body, uint16_t len, MinimedIddStatus *out);

//! Parse a decrypted TAS response: opcode 0x03FE + 16-bit flags + flag-gated fields consumed in
//! tas.py's exact order (auto-mode shield+readiness, PLGM, LGS, temp target, wait-to-calibrate,
//! safe basal). Trailing bytes are tolerated (matches the bridge). Returns false on a short body
//! or wrong opcode.
bool minimed_status_parse_tas(const uint8_t *body, uint16_t len, MinimedTas *out);

//! Feed the latest reads into the label state. Either input may be invalid -- its clauses just
//! don't fire; if BOTH are invalid the previous state is kept unchanged (bridge behaviour).
//! Stamps/clears the countdown epochs: warm-up expiry is set only on *entry* (a fixed self-timed
//! 2 h -- the pump rejects the warm-up-time-remaining opcode), suspend start on entry, temp-target
//! expiry restamped from the pump's live remaining-minutes on every read.
void minimed_status_update(const MinimedIddStatus *st, const MinimedTas *tas, uint32_t now);

//! Compose the current display string for watchface key 15: the label, time-augmented when
//! applicable ("WARM-UP 1:59" / "TEMP TARGET 0:45" countdowns, "SUSPENDED 0:12" count-up).
//! "" = normal (the watchface hides the band). Returns false if update() has never run with
//! valid data -- nothing should be sent yet.
bool minimed_status_compose(uint32_t now, char *out, uint16_t cap);

//! True while the active label carries a time component, i.e. the caller should re-compose and
//! re-send every minute so the countdown ticks.
bool minimed_status_ticking(void);

//! True while the SG is off the sensor scale (SensorMessageState SG below / above limit). The
//! read path shows "LO"/"HI" as the BG instead of the 0 mg/dL marker the pump sends, and the
//! band stays empty so it doesn't cover the graph.
bool minimed_status_sg_below(void);
bool minimed_status_sg_above(void);

//! True when the pump currently has no valid glucose (GST signal lost, or the sensor is in a
//! warm-up/searching/absent state): the watch should show "---" now rather than an aging number.
//! Only updated when the IDD Status read succeeded; retains its previous value otherwise.
bool minimed_status_bg_invalid(void);

//! Reset all internal state (label + epochs). For the host tests; the firmware deliberately
//! keeps state across reconnects so e.g. a warm-up countdown survives a pump dropout.
void minimed_status_reset(void);
