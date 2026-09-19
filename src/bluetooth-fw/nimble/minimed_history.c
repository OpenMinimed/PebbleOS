/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_history.h"

#define REC_HEADER_LEN 8
// Time offset(2) + SG value(2). ISIG and V counter behind them are ignored, so a record truncated
// after the value still parses.
#define SG_MIN_LEN (REC_HEADER_LEN + 4)

#define EVENT_SG_MEASUREMENT 0xf00c

// Anything from here up is a code, not a glucose value (real values are at most 400 mg/dL).
#define SG_SPECIAL_MIN 0x0300

static uint16_t prv_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool minimed_history_parse_sg(const uint8_t *rec, uint16_t len, MinimedHistSg *out) {
  if (len < SG_MIN_LEN || prv_u16(rec) != EVENT_SG_MEASUREMENT) return false;
  out->seq = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) | ((uint32_t)rec[4] << 16) |
             ((uint32_t)rec[5] << 24);
  out->offset_min = (int16_t)prv_u16(rec + 8);
  out->sg = prv_u16(rec + 10);
  return true;
}

int32_t minimed_history_sg_to_mgdl(uint16_t sg, int32_t floor_mgdl, int32_t ceiling_mgdl) {
  if (sg == MINIMED_HIST_SG_BELOW) return floor_mgdl;
  if (sg == MINIMED_HIST_SG_ABOVE) return ceiling_mgdl;
  if (sg == 0 || sg >= SG_SPECIAL_MIN) return -1;
  return sg;
}
