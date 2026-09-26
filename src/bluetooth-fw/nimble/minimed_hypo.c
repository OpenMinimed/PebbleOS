/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

// Column order and arithmetic mirror sugar_predictor's features.py::build() and hypo.py's
// extra_features() exactly; the host test (tools/minimed_sake_hosttest) checks this against
// vectors generated from the numpy model.

#include "minimed_hypo.h"

#include <math.h>
#include <string.h>

#include "minimed_hypo_model.h"

_Static_assert(HYPO_HIST == MINIMED_PREDICT_WINDOW, "hypo model window size must match the predictor's");
_Static_assert(HYPO_STEP == MINIMED_PREDICT_CELL_SECS / 60, "hypo model cell size must match the predictor's");

#define NEW (HYPO_HIST - 1)  // index of the newest cell

// features.py _shift_past: drop the newest cell and repeat the oldest, keeping the window length.
// Position 0 of the shifted series holds series[0] twice, and everything else moves one cell
// later. Padding with zero instead shifts every activity kernel by about 0.001, which is 0.01
// standard deviations once the column is standardized -- enough to move the nadir head by
// 0.85 mg/dL (see sugar_predictor/INTEGRATION.md). Operates directly on the predictor's unshifted
// window, same as minimed_predict.c's own prv_shift_past.
static inline float prv_past(const float *series, int from_new) {
  int idx = NEW - from_new - 1;
  return idx < 0 ? series[0] : series[idx];
}

static inline float prv_lag(const float *bg, int k) {  // features.py: lag(k) = bg[-k]
  int idx = HYPO_HIST - k;
  return idx < 0 ? bg[0] : bg[idx];
}

// Abramowitz & Stegun 7.1.26. Max absolute error 1.5e-7, far below the spread of anything it is
// used on here, and costs no table.
static float prv_std_normal_cdf(float z) {
  static const float a1 = 0.254829592f, a2 = -0.284496736f, a3 = 1.421413741f;
  static const float a4 = -1.453152027f, a5 = 1.061405429f, p = 0.3275911f;
  float x = z / 1.41421356237f;
  const float sign = x < 0.0f ? -1.0f : 1.0f;
  x = fabsf(x);
  const float t = 1.0f / (1.0f + p * x);
  const float y = 1.0f - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * expf(-x * x);
  return 0.5f * (1.0f + sign * y);
}

bool minimed_hypo_should_evaluate(const MinimedPredictWindow *in) {
  return in->bg[NEW] <= HYPO_TRIGGER && in->bg[NEW] < prv_lag(in->bg, 4);
}

// The physiological ceiling this is anchored on: CGM systems' own steepest trend-arrow bucket
// (Dexcom "double down", Medtronic Guardian's fastest arrow) is >=3 mg/dL/min -- the top category
// these devices report at all, because interstitial sensor lag makes anything faster hard to
// resolve reliably (Klonoff & Kerr 2017, PMC5951054; FreeStyle Libre and Medtronic Guardian
// trend-arrow documentation agree on the same ~3 mg/dL/min top bucket). A 30-minute run at that
// rate is also still slower than clinical guidance for how fast a blood glucose is ever
// deliberately driven down (DKA correction: 90-120 mg/dL/h = 1.5-2 mg/dL/min is the RECOMMENDED
// max, i.e. even that sanctioned rate is gentler than this). 3 mg/dL/min is therefore a defensible
// ceiling for "as fast as glucose realistically falls", not a guess.
#define HYPO_FAST_FALL_MGDL_PER_MIN 3.0f
#define HYPO_EARLY_LOOKAHEAD_MIN 30.0f
// Never evaluate the model more than this far above its own fitted trigger: bounds how far the
// early gate below extrapolates the input away from where the model was fit, regardless of how
// steep the observed slope claims to be (a single noisy or stale cell must not imply an
// arbitrarily large jump).
#define HYPO_EARLY_CEILING_MGDL 130.0f

bool minimed_hypo_falling_fast(const MinimedPredictWindow *in) {
  const float now = in->bg[NEW];
  if (now <= HYPO_TRIGGER || now > HYPO_EARLY_CEILING_MGDL) {
    return false;  // already handled by should_evaluate, or too far above the model's regime
  }
  const float l4 = prv_lag(in->bg, 4);  // 15 minutes back
  const float slope_per_min = (now - l4) / 15.0f;  // negative while falling
  if (slope_per_min > -HYPO_FAST_FALL_MGDL_PER_MIN) {
    return false;  // not falling at the physiological worst-case rate: an ordinary decline
  }
  const float projected = now + slope_per_min * HYPO_EARLY_LOOKAHEAD_MIN;
  return projected <= HYPO_LOW;
}

static void prv_features(const MinimedPredictWindow *in, float out[HYPO_N_IN]) {
  const float *bg = in->bg;
  const float now = bg[NEW];

  const float d5 = now - prv_lag(bg, 2);
  const float d10 = now - prv_lag(bg, 3);
  const float d15 = now - prv_lag(bg, 4);
  const float d30 = now - prv_lag(bg, 7);
  const float d60 = now - prv_lag(bg, 13);
  const float d90 = now - prv_lag(bg, 19);
  const float d120 = now - prv_lag(bg, 25);
  const float acc5 = d5 - (prv_lag(bg, 2) - prv_lag(bg, 3));
  const float acc10 = d10 - (prv_lag(bg, 3) - prv_lag(bg, 5));

  // rolling statistics over the last 12 and 36 cells
  float s12 = 0.0f, s36 = 0.0f, lo12 = bg[NEW], hi12 = bg[NEW];
  for (int k = 0; k < 36; k++) {
    const float v = bg[HYPO_HIST - 36 + k];
    s36 += v;
    if (k >= 24) {
      s12 += v;
      if (v < lo12) lo12 = v;
      if (v > hi12) hi12 = v;
    }
  }
  const float m12 = s12 / 12.0f, m36 = s36 / 36.0f;
  float v12 = 0.0f, v36 = 0.0f;
  for (int k = 0; k < 36; k++) {
    const float v = bg[HYPO_HIST - 36 + k];
    v36 += (v - m36) * (v - m36);
    if (k >= 24) v12 += (v - m12) * (v - m12);
  }
  const float std12 = sqrtf(v12 / 12.0f);
  const float std36 = sqrtf(v36 / 36.0f);
  const float rng12 = hi12 - lo12;

  // activity kernels over the shifted series, oldest cell first
  float iob[3] = {0.0f, 0.0f, 0.0f}, cob[3] = {0.0f, 0.0f, 0.0f};
  const float *ik[3] = {hypo_k_ins0, hypo_k_ins1, hypo_k_ins2};
  const float *ck[3] = {hypo_k_carb0, hypo_k_carb1, hypo_k_carb2};
  for (int c = 0; c < HYPO_HIST; c++) {
    const int from_new = NEW - c;  // 0 = newest
    const float i_v = prv_past(in->ins, from_new);
    const float c_v = prv_past(in->carb, from_new);
    for (int j = 0; j < 3; j++) {
      iob[j] += i_v * ik[j][c];
      cob[j] += c_v * ck[j][c];
    }
  }

  // minutes since the last delivery / carb entry, capped at 240, and the largest carb entry in
  // the window -- all on the shifted series
  float t_ins = (float)HYPO_HIST * HYPO_STEP, t_carb = t_ins;
  float largest_carb = 0.0f;
  for (int from_new = 0; from_new < HYPO_HIST; from_new++) {
    const float i_v = prv_past(in->ins, from_new);
    const float c_v = prv_past(in->carb, from_new);
    if (i_v > 0.3f && from_new * HYPO_STEP < t_ins) t_ins = (float)from_new * HYPO_STEP;
    if (c_v > 0.0f && from_new * HYPO_STEP < t_carb) t_carb = (float)from_new * HYPO_STEP;
    if (c_v > largest_carb) largest_carb = c_v;
  }
  if (t_ins > 240.0f) t_ins = 240.0f;
  if (t_carb > 240.0f) t_carb = 240.0f;

  int i = 0;
  out[i++] = 1.0f;  // bias
  out[i++] = now;
  out[i++] = d5;
  out[i++] = d10;
  out[i++] = d15;
  out[i++] = d30;
  out[i++] = d60;
  out[i++] = d90;
  out[i++] = d120;
  out[i++] = acc5;
  out[i++] = acc10;
  out[i++] = std12;
  out[i++] = rng12;
  out[i++] = std36;
  out[i++] = iob[0];
  out[i++] = iob[1];
  out[i++] = iob[2];
  out[i++] = cob[0];
  out[i++] = cob[1];
  out[i++] = cob[2];
  out[i++] = t_ins / 60.0f;
  out[i++] = t_carb / 60.0f;
  out[i++] = largest_carb / 10.0f;
  out[i++] = fabsf(d5);
  out[i++] = d5 * d10 / 10.0f;
  out[i++] = now * d5 / 100.0f;
  out[i++] = now - 180.0f > 0.0f ? now - 180.0f : 0.0f;
  out[i++] = 70.0f - now > 0.0f ? 70.0f - now : 0.0f;
  out[i++] = d30 * d5 / 10.0f;
  out[i++] = (now - 120.0f) * (now - 120.0f) / 1000.0f;
  out[i++] = (d5 > 0.0f ? 1.0f : (d5 < 0.0f ? -1.0f : 0.0f)) * d5 * d5 / 10.0f;
  out[i++] = now * d30 / 100.0f;
  out[i++] = expf(-t_carb / 60.0f) * largest_carb / 10.0f;
  out[i++] = in->tod_sin;
  out[i++] = in->tod_cos;

  // hypo.py extra_features(): six columns aimed at a falling low
  float recent_ins = 0.0f, mean_cell = 0.0f;
  for (int from_new = 0; from_new < HYPO_HIST - 1; from_new++) {
    const float v = prv_past(in->ins, from_new);
    mean_cell += v;
    if (from_new < 6) recent_ins += v;
  }
  mean_cell /= (float)(HYPO_HIST - 1);
  float suppression = recent_ins / (6.0f * mean_cell + 1e-6f);
  if (suppression > 4.0f) suppression = 4.0f;
  if (suppression < 0.0f) suppression = 0.0f;

  float since_ins = (float)(HYPO_HIST - 1);
  for (int from_new = 0; from_new < HYPO_HIST - 1; from_new++) {
    if (prv_past(in->ins, from_new) > 0.01f) {
      since_ins = (float)from_new;
      break;
    }
  }
  float hours_since = since_ins * HYPO_STEP / 60.0f;
  if (hours_since > 4.0f) hours_since = 4.0f;

  float mins_low_hist = 0.0f, floor12 = bg[NEW];
  for (int k = HYPO_HIST - 12; k < HYPO_HIST; k++) {
    if (bg[k] < HYPO_LOW) mins_low_hist += HYPO_STEP;
    if (bg[k] < floor12) floor12 = bg[k];
  }

  out[i++] = recent_ins;
  out[i++] = suppression;
  out[i++] = hours_since;
  out[i++] = mins_low_hist;
  out[i++] = floor12 / 100.0f;
  out[i++] = (now - floor12) / 10.0f;
}

// Per-head tables, indexed by hypo_head_t. Kept here so minimed_hypo_model.h stays a plain data
// file.
typedef struct {
  const int8_t *w1;  // [wide][HYPO_N_IN], row-major per hidden unit
  const float *b1;
  const int8_t *w2;
  const float *mu;
  const float *sd;
  int wide;
  float sw1, sw2, b2, ym, ys;
} HeadDesc;

#define DESC(idx, tag)                                                                            \
  {hypo_h##idx##_w1, hypo_h##idx##_b1, hypo_h##idx##_w2, hypo_h##idx##_mu, hypo_h##idx##_sd,      \
   HYPO_H##idx##_WIDE, HYPO_H##idx##_SW1, HYPO_H##idx##_SW2, HYPO_H##idx##_B2, HYPO_H##idx##_YM,  \
   HYPO_H##idx##_YS}

static const HeadDesc HEADS[HYPO_N_HEADS] = {
    DESC(0, base),    DESC(1, uplift),      DESC(2, base_mins), DESC(3, uplift_mins),
    DESC(4, base_peak), DESC(5, uplift_peak), DESC(6, risk_low),  DESC(7, risk_severe)};

static float prv_head(hypo_head_t head, const float feat[HYPO_N_IN]) {
  const HeadDesc *d = &HEADS[head];
  float z[HYPO_N_IN];
  z[0] = feat[0];  // the bias is not scaled
  for (int k = 1; k < HYPO_N_IN; k++) z[k] = (feat[k] - d->mu[k]) / d->sd[k];

  float acc = d->b2;
  for (int u = 0; u < d->wide; u++) {
    const int8_t *row = d->w1 + (size_t)u * HYPO_N_IN;
    float h = d->b1[u];
    for (int k = 0; k < HYPO_N_IN; k++) h += z[k] * (float)row[k] * d->sw1;
    if (h > 0.0f) acc += h * (float)d->w2[u] * d->sw2;
  }
  return acc * d->ys + d->ym;
}

void minimed_hypo_eval(const MinimedPredictWindow *in, float over_treat_weight,
                       MinimedHypoPrediction *out) {
  float feat[HYPO_N_IN];
  prv_features(in, feat);

  const float nu = prv_head(HYPO_BASE, feat);
  float up = prv_head(HYPO_UPLIFT, feat);
  if (up < 0.0f) up = 0.0f;  // carbs cannot lower glucose

  float mu_ = prv_head(HYPO_BASE_MINS, feat);
  if (mu_ < 0.0f) mu_ = 0.0f;
  float up_m = prv_head(HYPO_UPLIFT_MINS, feat);
  if (up_m > 0.0f) up_m = 0.0f;  // nor lengthen a low
  float mt = mu_ + up_m;
  if (mt < 0.0f) mt = 0.0f;

  const float pu = prv_head(HYPO_BASE_PEAK, feat);
  float up_p = prv_head(HYPO_UPLIFT_PEAK, feat);
  if (up_p < 0.0f) up_p = 0.0f;

  float p_low = prv_head(HYPO_RISK_LOW, feat);
  float p_sev = prv_head(HYPO_RISK_SEVERE, feat);
  if (p_low < 1e-4f) p_low = 1e-4f;
  if (p_low > 1.0f - 1e-4f) p_low = 1.0f - 1e-4f;
  if (p_sev < 1e-4f) p_sev = 1e-4f;
  if (p_sev > 1.0f - 1e-4f) p_sev = 1.0f - 1e-4f;

  const float p_over = 1.0f - prv_std_normal_cdf((HYPO_OVER - (pu + up_p)) / HYPO_SIGMA_PK);

  float pct = 100.0f * (p_low - over_treat_weight * p_over);
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;

  out->nadir_untreated = nu;
  out->nadir_treated = nu + up;
  out->mins_untreated = mu_;
  out->mins_treated = mt;
  out->mins_saved = mu_ - mt;
  out->p_low = p_low;
  out->p_severe = p_sev;
  out->p_over = p_over;
  out->treat_pct = pct;
}
