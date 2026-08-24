/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_annunciation.h"

#include <stddef.h>

// Record header: event type(2) + sequence number(4) + relative offset(2), all little-endian.
#define REC_HEADER_LEN 8
// Consolidated event data through the status byte: flags(1) + id(2) + type(2) + status(1).
// The u32 timestamp and the type-dependent aux fields behind it are ignored, so a record
// truncated after status still parses (and a stray E2E trailer is harmlessly skipped).
#define ANNUNC_MIN_LEN (REC_HEADER_LEN + 6)

#define EVENT_ANNUNCIATION_CONSOLIDATED 0xf010

static uint16_t prv_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

MinimedAnnuncRecord minimed_annunciation_parse_record(const uint8_t *rec, uint16_t len,
                                                      MinimedAnnunciation *out) {
  if (len < REC_HEADER_LEN) return MinimedAnnuncRecordBad;
  out->seq = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) | ((uint32_t)rec[4] << 16) |
             ((uint32_t)rec[5] << 24);
  if (prv_u16(rec) != EVENT_ANNUNCIATION_CONSOLIDATED) return MinimedAnnuncRecordOther;
  if (len < ANNUNC_MIN_LEN) return MinimedAnnuncRecordBad;
  const uint8_t flags = rec[8];
  const uint16_t type_raw = prv_u16(rec + 11);
  // The raw type always carries the 0xf000 nibble (data.py); anything else is a garbled read.
  if ((type_raw & 0xf000) != 0xf000) return MinimedAnnuncRecordBad;
  out->id = prv_u16(rec + 9);
  out->type = type_raw & 0x0fff;
  out->status = rec[13];
  out->silenced = (flags & 0x40) != 0;  // AnnunciationEventFlag.ALERT_SILENCED
  return MinimedAnnuncRecordYes;
}

// Display names, ported from PythonPumpConnector AnnunciationType (several upstream names are
// themselves guesses from pump alert text). Rename entries to the pump's exact wording as codes
// are observed on HW -- field-confirmed so far: 0x054 (bridge, 2026-07-20), 0x325 (2026-08-19).
// Codes not listed fall back to the caller's hex label -- mirror-everything, never drop.
typedef struct {
  uint16_t type;
  const char *name;
} AnnuncName;

static const AnnuncName s_names[] = {
    {0x007, "No delivery"},
    {0x033, "Bolus stopped"},
    {0x047, "Max fill reached"},
    {0x048, "Max fill reached"},
    {0x054, "Insert battery"},
    {0x067, "Check bolus BG"},
    {0x068, "Low pump battery"},
    {0x069, "Low reservoir"},
    {0x06a, "Low reservoir"},
    {0x06c, "Reminder"},
    {0x06d, "Set change reminder"},
    {0x075, "IOB cleared"},
    {0x307, "Calibrate now"},
    {0x308, "Calibration not accepted"},
    {0x309, "Change sensor"},
    {0x30a, "Change sensor"},
    {0x30c, "Lost sensor signal"},
    {0x315, "Change sensor"},
    {0x31e, "Sensor connected"},
    {0x321, "Sensor error"},
    {0x322, "Low SG"},
    {0x323, "Low SG suspend"},
    {0x325, "Alert before low"},  // pump wording, HW-confirmed 2026-08-19
    {0x327, "Predictive resume"},
    {0x329, "Threshold suspend"},
    {0x32a, "Suspend before low"},
    {0x32b, "Suspend before low"},
    {0x32e, "Suspend timeout"},
    {0x32f, "Delivery resumed"},
    {0x330, "High SG"},
    {0x331, "High SG"},
    {0x333, "SmartGuard exit (high SG)"},
    {0x334, "SmartGuard exit"},
    {0x335, "SmartGuard min delivery"},
    {0x336, "SmartGuard max delivery"},
    {0x33a, "SmartGuard off"},
    {0x33b, "Severe low SG"},
    {0x341, "Bolus recommended"},
    {0x344, "High SG 3 h"},
    {0x345, "Calibration recommended"},
    {0x34b, "Calibration OK"},
    {0x34c, "Early calibration"},
    {0x365, "Calibrate reminder"},
};

const char *minimed_annunciation_name(uint16_t type) {
  for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
    if (s_names[i].type == type) return s_names[i].name;
  }
  return NULL;
}
