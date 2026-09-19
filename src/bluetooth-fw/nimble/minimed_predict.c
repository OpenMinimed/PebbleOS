/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_predict.h"

#include <math.h>
#include <string.h>

#include "minimed_predict_model.h"

#define WINDOW MINIMED_PREDICT_WINDOW
#define CELLS MINIMED_PREDICT_CELLS
#define STALE_SECS (10 * 60)

// -- Model (ported from sugar_predictor/firmware/predict.c) --------------------------------------

typedef MinimedPredictWindow ModelInput;

static float prv_std_of(const float *a, int n) {
  float m = 0.0f, s = 0.0f;
  for (int i = 0; i < n; i++) m += a[i];
  m /= (float)n;
  for (int i = 0; i < n; i++) s += (a[i] - m) * (a[i] - m);
  return sqrtf(s / (float)n);
}

static float prv_dot_k(const float *a, const float *k) {
  float s = 0.0f;
  for (int i = 0; i < BGP_HIST; i++) s += a[i] * k[i];
  return s;
}

// Minutes since the newest cell that passes the threshold, capped at 240.
static float prv_since(const float *a, float thr) {
  for (int i = BGP_HIST - 1; i >= 0; i--) {
    if (a[i] > thr) {
      const float m = (float)(BGP_HIST - 1 - i) * 5.0f;
      return m > 240.0f ? 240.0f : m;
    }
  }
  return 240.0f;
}

// The newest insulin/carb cell covers [t, t+5) and is not observable at t, so the window is shifted
// one cell into the past, repeating the oldest cell (as the training features do).
static void prv_shift_past(const float *src, float *dst) {
  dst[0] = src[0];
  for (int i = 1; i < BGP_HIST; i++) dst[i] = src[i - 1];
}

static void prv_build_features(const ModelInput *in, float *f) {
  const float *bg = in->bg;
  float ins[BGP_HIST], carb[BGP_HIST];
  const float now = bg[BGP_HIST - 1];
  // lag(k) in the training feature builder is bg[HIST - k]
  const float l2 = bg[BGP_HIST - 2], l3 = bg[BGP_HIST - 3], l4 = bg[BGP_HIST - 4];
  const float l5 = bg[BGP_HIST - 5], l7 = bg[BGP_HIST - 7], l13 = bg[BGP_HIST - 13];
  const float l19 = bg[BGP_HIST - 19], l25 = bg[BGP_HIST - 25];
  const float d5 = now - l2, d10 = now - l3, d15 = now - l4, d30 = now - l7;
  const float d60 = now - l13, d90 = now - l19, d120 = now - l25;
  float mx, mn, last_carb = 0.0f;

  prv_shift_past(in->ins, ins);
  prv_shift_past(in->carb, carb);

  mx = mn = bg[BGP_HIST - 12];
  for (int i = BGP_HIST - 12; i < BGP_HIST; i++) {
    if (bg[i] > mx) mx = bg[i];
    if (bg[i] < mn) mn = bg[i];
  }
  for (int i = 0; i < BGP_HIST; i++) {
    if (carb[i] > last_carb) last_carb = carb[i];
  }
  const float t_ins = prv_since(ins, 0.3f);
  const float t_carb = prv_since(carb, 0.0f);

  f[0] = 1.0f;
  f[1] = now;
  f[2] = d5;
  f[3] = d10;
  f[4] = d15;
  f[5] = d30;
  f[6] = d60;
  f[7] = d90;
  f[8] = d120;
  f[9] = d5 - (l2 - l3);
  f[10] = d10 - (l3 - l5);
  f[11] = prv_std_of(bg + BGP_HIST - 12, 12);
  f[12] = mx - mn;
  f[13] = prv_std_of(bg + BGP_HIST - 36, 36);
  f[14] = prv_dot_k(ins, bgp_kins0);
  f[15] = prv_dot_k(ins, bgp_kins1);
  f[16] = prv_dot_k(ins, bgp_kins2);
  f[17] = prv_dot_k(carb, bgp_kcarb0);
  f[18] = prv_dot_k(carb, bgp_kcarb1);
  f[19] = prv_dot_k(carb, bgp_kcarb2);
  f[20] = t_ins / 60.0f;
  f[21] = t_carb / 60.0f;
  f[22] = last_carb / 10.0f;
  f[23] = d5 < 0.0f ? -d5 : d5;
  f[24] = d5 * d10 / 10.0f;
  f[25] = now * d5 / 100.0f;
  f[26] = now > 180.0f ? now - 180.0f : 0.0f;
  f[27] = now < 70.0f ? 70.0f - now : 0.0f;
  f[28] = d30 * d5 / 10.0f;
  f[29] = (now - 120.0f) * (now - 120.0f) / 1000.0f;
  f[30] = (d5 > 0.0f ? 1.0f : (d5 < 0.0f ? -1.0f : 0.0f)) * d5 * d5 / 10.0f;
  f[31] = now * d30 / 100.0f;
  f[32] = expf(-t_carb / 60.0f) * last_carb / 10.0f;
  f[33] = in->tod_sin;
  f[34] = in->tod_cos;
  f[35] = in->dow_sin;
  f[36] = in->dow_cos;
  int hour = in->hour;
  if (hour < 0) hour = 0;
  if (hour > 23) hour = 23;
  f[37] = bgp_hour_base[hour];
}

static float prv_model_predict(const float *f_raw) {
  float f[BGP_DIN];
  float out = bgp_b2;
  for (int i = 0; i < BGP_DIN; i++) f[i] = (f_raw[i] - bgp_mu[i]) / bgp_sd[i];
  for (int j = 0; j < BGP_HID; j++) {
    const signed char *w = bgp_w1 + (long)j * BGP_DIN;
    float acc = 0.0f;
    for (int i = 0; i < BGP_DIN; i++) acc += (float)w[i] * f[i];
    acc = acc * bgp_s_w1 + (float)bgp_b1[j] * bgp_s_b1;
    if (acc > 0.0f) out += acc * (float)bgp_w2[j] * bgp_s_w2;
  }
  return out * bgp_ys + bgp_ym;  // delta over the current reading
}

static float prv_model_low_probability(const float *f_raw) {
  float z = 0.0f;
  for (int i = 0; i < BGP_DIN; i++) z += ((f_raw[i] - bgp_mu[i]) / bgp_sd[i]) * bgp_w_low[i];
  return 1.0f / (1.0f + expf(-z));
}

void minimed_predict_window(const MinimedPredictWindow *in, MinimedPrediction *out) {
  float f[BGP_DIN];
  prv_build_features(in, f);
  out->mgdl = in->bg[WINDOW - 1] + prv_model_predict(f);
  out->low_prob = prv_model_low_probability(f);
  out->low_alarm = out->low_prob >= bgp_thr_low;
}

// -- State ----------------------------------------------------------------------------------------

void minimed_predict_reset(MinimedPredictState *st) { memset(st, 0, sizeof(*st)); }

static int prv_slot(int32_t cell) {
  const int m = (int)(cell % CELLS);
  return m < 0 ? m + CELLS : m;
}

static void prv_clear_slot(MinimedPredictState *st, int32_t cell) {
  const int s = prv_slot(cell);
  st->bg[s] = 0.0f;
  st->bg_set[s] = false;
  st->ins[s] = 0.0f;
  st->carb[s] = 0.0f;
}

// Cell index of a timestamp, snapped to the nearest 5-minute mark like the training grid.
static int32_t prv_cell_of(uint32_t ts) { return (int32_t)((ts + MINIMED_PREDICT_CELL_SECS / 2) / MINIMED_PREDICT_CELL_SECS); }

// The slot for a cell, advancing the ring if the cell is newer than any so far. NULL-equivalent
// (-1) for a cell that has already aged out of the ring.
static int prv_touch(MinimedPredictState *st, int32_t cell) {
  if (!st->have) {
    st->have = true;
    st->head = cell;
    prv_clear_slot(st, cell);
  } else if (cell > st->head) {
    const int32_t from = (cell - st->head >= CELLS) ? cell - CELLS + 1 : st->head + 1;
    for (int32_t c = from; c <= cell; c++) prv_clear_slot(st, c);
    st->head = cell;
  } else if (cell <= st->head - CELLS) {
    return -1;
  }
  return prv_slot(cell);
}

void minimed_predict_add_bg(MinimedPredictState *st, uint32_t ts, int32_t mgdl) {
  if (mgdl <= 0) return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s < 0) return;
  st->bg[s] = (float)mgdl;
  st->bg_set[s] = true;
}

void minimed_predict_add_insulin(MinimedPredictState *st, uint32_t ts, float units) {
  if (units <= 0.0f) return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s >= 0) st->ins[s] += units;
}

void minimed_predict_add_carbs(MinimedPredictState *st, uint32_t ts, float grams) {
  if (grams <= 0.0f) return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s >= 0) st->carb[s] += grams;
}

void minimed_predict_set_basal(MinimedPredictState *st, uint32_t ts, float u_per_h) {
  st->basal_u_per_h = u_per_h > 0.0f ? u_per_h : 0.0f;
  st->basal_from = prv_cell_of(ts);
}

// -- Run ------------------------------------------------------------------------------------------

static float prv_cell_bg(const MinimedPredictState *st, int32_t cell, bool *set) {
  *set = false;
  if (cell > st->head || cell <= st->head - CELLS) return 0.0f;
  const int s = prv_slot(cell);
  *set = st->bg_set[s];
  return st->bg[s];
}

bool minimed_predict_run(const MinimedPredictState *st, uint32_t now, int32_t gmt_offset_secs,
                         MinimedPrediction *out) {
  if (!st->have) return false;

  // The newest cell that holds a reading.
  int32_t newest = st->head;
  bool set = false;
  while (newest > st->head - CELLS) {
    prv_cell_bg(st, newest, &set);
    if (set) break;
    newest--;
  }
  if (!set) return false;
  if ((int64_t)now - (int64_t)newest * MINIMED_PREDICT_CELL_SECS > STALE_SECS +
                                                                    MINIMED_PREDICT_CELL_SECS / 2) {
    return false;
  }

  ModelInput in;
  const int32_t first = newest - (WINDOW - 1);
  // BG: forward-fill gaps (the pump keeps showing its last reading), and repeat the oldest known
  // reading into any leading cells that were never seen.
  float last = 0.0f;
  bool have_last = false;
  for (int k = 0; k < WINDOW; k++) {
    bool s;
    const float v = prv_cell_bg(st, first + k, &s);
    if (s) {
      last = v;
      have_last = true;
    }
    in.bg[k] = have_last ? last : 0.0f;
  }
  int lead = 0;
  while (lead < WINDOW && in.bg[lead] == 0.0f) lead++;
  for (int k = 0; k < lead && lead < WINDOW; k++) in.bg[k] = in.bg[lead];

  for (int k = 0; k < WINDOW; k++) {
    const int32_t cell = first + k;
    in.ins[k] = 0.0f;
    in.carb[k] = 0.0f;
    if (cell <= st->head && cell > st->head - CELLS) {
      in.ins[k] = st->ins[prv_slot(cell)];
      in.carb[k] = st->carb[prv_slot(cell)];
    }
    if (st->basal_u_per_h > 0.0f && cell >= st->basal_from) {
      in.ins[k] += st->basal_u_per_h * (MINIMED_PREDICT_CELL_SECS / 3600.0f);
    }
  }

  // Local time. Day 0 of the epoch was a Thursday, and the model's day-of-week index counts from
  // there (Thursday = 0), so days-since-epoch mod 7 is used as is.
  const int64_t local = (int64_t)now + gmt_offset_secs;
  const int64_t days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
  const int64_t sec_of_day = local - days * 86400;
  const int dow = (int)(((days % 7) + 7) % 7);
  const float minutes = (float)sec_of_day / 60.0f;
  const float two_pi = 6.28318530717958647692f;
  in.tod_sin = sinf(two_pi * minutes / 1440.0f);
  in.tod_cos = cosf(two_pi * minutes / 1440.0f);
  in.dow_sin = sinf(two_pi * (float)dow / 7.0f);
  in.dow_cos = cosf(two_pi * (float)dow / 7.0f);
  in.hour = (int)(sec_of_day / 3600);

  minimed_predict_window(&in, out);
  return true;
}
