/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_history.h"

#define REC_HEADER_LEN 8
// Time offset(2) + SG value(2). ISIG and V counter behind them are ignored, so a record truncated
// after the value still parses.
#define SG_MIN_LEN (REC_HEADER_LEN + 4)

#define EVENT_BOLUS_DELIVERED_P1 0x0069
#define EVENT_BASAL_RATE_CHANGED 0x0099
#define EVENT_AUTO_BASAL 0xf001
#define BASAL_CONTEXT_PRESENT 0x01
#define BASAL_CONTEXT_ALGORITHM 0x55
#define EVENT_MEAL 0xf005
#define EVENT_NGP_REFERENCE_TIME 0xf00e
// Recording reason(1) + date time: year(2) month(1) day(1) hours(1) minutes(1) seconds(1)
#define REF_MIN_LEN (REC_HEADER_LEN + 8)
#define EVENT_SG_MEASUREMENT 0xf00c
// Food amount(2)
#define MEAL_MIN_LEN (REC_HEADER_LEN + 2)

// Anything from here up is a code, not a glucose value (real values are at most 400 mg/dL).
#define SG_SPECIAL_MIN 0x0300

static uint16_t prv_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Days since 2000-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
static int32_t prv_days_from_civil(int32_t y, int32_t m, int32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const int32_t yoe = y - era * 400;
  const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468 - 10957;  // 10957 days from 1970 to 2000
}

bool minimed_history_parse_ref_time(const uint8_t *rec, uint16_t len, MinimedHistRef *out) {
  if (len < REF_MIN_LEN || prv_u16(rec) != EVENT_NGP_REFERENCE_TIME) return false;
  const uint16_t year = prv_u16(rec + 9);
  const uint8_t month = rec[11], day = rec[12], hour = rec[13], min = rec[14], sec = rec[15];
  if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      min > 59 || sec > 59) {
    return false;
  }
  out->seq = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) | ((uint32_t)rec[4] << 16) |
             ((uint32_t)rec[5] << 24);
  out->secs = (uint32_t)prv_days_from_civil(year, month, day) * 86400u + hour * 3600u + min * 60u +
              sec;
  return true;
}

uint32_t minimed_history_sg_secs(const MinimedHistRef *ref, const MinimedHistSg *sg) {
  return ref->secs + (uint32_t)((int32_t)sg->offset_min * 60);
}

// IEEE-11073 FLOAT (MedFloat32): a signed 24-bit mantissa under a signed 8-bit decimal exponent.
static float prv_medfloat32(const uint8_t *p) {
  int32_t mant = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
  if (mant & 0x800000) mant -= 0x1000000;
  const int exp = (int8_t)p[3];
  // NaN/NRes/Infinity codes (mantissa 0x7FFFFF, 0x800000, 0x7FFFFE, 0x800001, 0x800002)
  if ((p[2] == 0x7f && p[1] == 0xff && (p[0] == 0xff || p[0] == 0xfe)) ||
      (p[2] == 0x80 && p[1] == 0x00 && p[0] <= 0x02)) {
    return -1.0f;
  }
  double v = (double)mant;
  for (int e = exp; e > 0; e--) v *= 10.0;
  for (int e = exp; e < 0; e++) v /= 10.0;
  return (float)v;
}

bool minimed_history_parse_insulin(const uint8_t *rec, uint16_t len, MinimedHistInsulin *out) {
  if (len < REC_HEADER_LEN) return false;
  const uint16_t type = prv_u16(rec);
  const uint32_t seq = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) | ((uint32_t)rec[4] << 16) |
                       ((uint32_t)rec[5] << 24);
  const uint8_t *d = rec + REC_HEADER_LEN;
  const uint16_t dlen = (uint16_t)(len - REC_HEADER_LEN);
  out->by_algorithm = false;
  if (type == EVENT_BOLUS_DELIVERED_P1) {
    // bolus id(2) type(1) fast(4) extended(4) duration(2)
    if (dlen < 11) return false;
    const float fast = prv_medfloat32(d + 3), ext = prv_medfloat32(d + 7);
    if (fast < 0.0f || ext < 0.0f) return false;
    out->kind = MinimedHistInsulinBolus;
    out->amount = fast + ext;
  } else if (type == EVENT_AUTO_BASAL) {
    // bolus number(1) amount(4)
    if (dlen < 5) return false;
    out->amount = prv_medfloat32(d + 1);
    if (out->amount < 0.0f) return false;
    out->kind = MinimedHistInsulinMicro;
  } else if (type == EVENT_BASAL_RATE_CHANGED) {
    // flags(1) old rate(4) new rate(4) [context(1)]
    if (dlen < 9) return false;
    out->amount = prv_medfloat32(d + 5);
    if (out->amount < 0.0f) return false;
    out->kind = MinimedHistInsulinBasalRate;
    out->by_algorithm = (d[0] & BASAL_CONTEXT_PRESENT) && dlen >= 10 && d[9] == BASAL_CONTEXT_ALGORITHM;
  } else {
    return false;
  }
  out->seq = seq;
  return true;
}

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

int minimed_history_sg_edge(uint16_t sg) {
  if (sg == MINIMED_HIST_SG_BELOW) return 1;
  if (sg == MINIMED_HIST_SG_ABOVE) return 2;
  return 0;
}

int32_t minimed_history_sg_to_mgdl(uint16_t sg, int32_t floor_mgdl, int32_t ceiling_mgdl) {
  if (sg == MINIMED_HIST_SG_BELOW) return floor_mgdl;
  if (sg == MINIMED_HIST_SG_ABOVE) return ceiling_mgdl;
  if (sg == 0 || sg >= SG_SPECIAL_MIN) return -1;
  return sg;
}

bool minimed_history_decode(MinimedHistClock *clk, const uint8_t *rec, uint16_t len,
                            int32_t floor_mgdl, int32_t ceiling_mgdl, MinimedHistEvent *out) {
  MinimedHistRef ref;
  if (minimed_history_parse_ref_time(rec, len, &ref)) {
    clk->ref = ref;
    clk->have_ref = true;
    out->kind = MinimedHistEventRef;
    out->seq = ref.seq;
    out->secs = ref.secs;
    return true;
  }
  if (!clk->have_ref || len < REC_HEADER_LEN) return false;
  const uint32_t rel_secs = clk->ref.secs + prv_u16(rec + 6);  // Relative Offset, seconds

  MinimedHistSg sg;
  MinimedHistMeal meal;
  MinimedHistInsulin ins;
  if (minimed_history_parse_sg(rec, len, &sg)) {
    if (sg.offset_min < 0) return false;
    out->kind = MinimedHistEventSg;
    out->seq = sg.seq;
    out->secs = minimed_history_sg_secs(&clk->ref, &sg);
    out->sg = sg.sg;
    out->mgdl = minimed_history_sg_to_mgdl(sg.sg, floor_mgdl, ceiling_mgdl);
    return true;
  }
  if (minimed_history_parse_meal(rec, len, &meal)) {
    if (meal.grams == 0) return false;
    out->kind = MinimedHistEventCarbs;
    out->seq = meal.seq;
    out->secs = rel_secs;
    out->value = (float)meal.grams;
    return true;
  }
  if (minimed_history_parse_insulin(rec, len, &ins)) {
    out->seq = ins.seq;
    out->secs = rel_secs;
    if (ins.kind == MinimedHistInsulinBasalRate) {
      out->kind = MinimedHistEventBasal;
      out->value = ins.by_algorithm ? 0.0f : ins.amount;
    } else {
      out->kind = MinimedHistEventInsulin;
      out->value = ins.amount;
    }
    return true;
  }
  return false;
}
