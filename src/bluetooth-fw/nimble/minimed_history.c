/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_history.h"

#define REC_HEADER_LEN 8
// Time offset(2) + SG value(2). ISIG and V counter behind them are ignored, so a record truncated
// after the value still parses.
#define SG_MIN_LEN (REC_HEADER_LEN + 4)

#define EVENT_MEAL 0xf005
#define EVENT_SG_MEASUREMENT 0xf00c
// Food amount(2)
#define MEAL_MIN_LEN (REC_HEADER_LEN + 2)

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

// IEEE-11073 SFLOAT (MedFloat16) to whole units, rounded half up. False for the NaN/NRes/Inf
// codes and for negative values.
static bool prv_sfloat_to_units(uint16_t raw, uint32_t *out) {
  const uint16_t m12 = raw & 0x0FFF;
  if (m12 >= 0x07FE && m12 <= 0x0802) return false;
  int exp = (raw >> 12) & 0x0F;
  if (exp & 0x8) exp -= 0x10;
  int32_t mant = m12;
  if (mant & 0x800) mant -= 0x1000;
  if (mant < 0) return false;
  int64_t v = mant;
  for (; exp > 0; exp--) v *= 10;
  int64_t div = 1;
  for (; exp < 0; exp++) div *= 10;
  v = (v + div / 2) / div;
  *out = (uint32_t)(v > 0xFFFF ? 0xFFFF : v);
  return true;
}

bool minimed_history_parse_meal(const uint8_t *rec, uint16_t len, MinimedHistMeal *out) {
  if (len < MEAL_MIN_LEN || prv_u16(rec) != EVENT_MEAL) return false;
  uint32_t grams;
  if (!prv_sfloat_to_units(prv_u16(rec + 8), &grams)) return false;
  out->seq = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) | ((uint32_t)rec[4] << 16) |
             ((uint32_t)rec[5] << 24);
  out->grams = (uint16_t)grams;
  return true;
}

int32_t minimed_history_sg_to_mgdl(uint16_t sg, int32_t floor_mgdl, int32_t ceiling_mgdl) {
  if (sg == MINIMED_HIST_SG_BELOW) return floor_mgdl;
  if (sg == MINIMED_HIST_SG_ABOVE) return ceiling_mgdl;
  if (sg == 0 || sg >= SG_SPECIAL_MIN) return -1;
  return sg;
}
