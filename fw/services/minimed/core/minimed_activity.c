/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_activity.h"

// Loop/oref exponential kernel, tp=55 min, td=360 min (LoopKit's Fiasp preset), sampled every
// 5 minutes and normalized so the 5-minute sum is 1. Units: 1e-6 per minute, so activity in
// milli-IU/min is a multiply-accumulate of d(mu) * K / 1e6. Generated from
//   tau = tp(1-tp/td)/(1-2tp/td),  a = 2tau/td,  S = 1/(1-a+(1+a)e^(-td/tau))
//   k(t) = (S/tau^2) * t * (1-t/td) * e^(-t/tau)
static const uint16_t s_kernel[MINIMED_ACTIVITY_TD_MIN / 5 + 1] = {
    0,    1605, 2938, 4032, 4918, 5622, 6169, 6579, 6871, 7063, 7169, 7201, 7172, 7092, 6969,
    6811, 6625, 6417, 6192, 5954, 5708, 5456, 5201, 4946, 4693, 4443, 4198, 3958, 3725, 3500,
    3282, 3073, 2873, 2681, 2498, 2324, 2159, 2002, 1854, 1715, 1583, 1459, 1342, 1233, 1131,
    1035, 946,  862,  785,  713,  646,  583,  526,  473,  423,  378,  336,  298,  263,  230,
    201,  174,  149,  126,  106,  87,   71,   55,   42,   29,   19,   9,    0,
};

static const int32_t KERNEL_SCALE = 1000000;  // K is 1e-6 per minute

void minimed_activity_init(MinimedActivity *a) {
  *a = (MinimedActivity){0};
}

static void prv_record(MinimedActivity *a, uint32_t ts, int32_t mu) {
  if (mu <= 0) return;
  a->ledger[a->head] = (MinimedActivityEntry){.ts = ts, .mu = mu};
  a->head = (uint16_t)((a->head + 1) % MINIMED_ACTIVITY_LEDGER_MAX);
  if (a->count < MINIMED_ACTIVITY_LEDGER_MAX) a->count++;
}

bool minimed_activity_observe_reservoir(MinimedActivity *a, int32_t res_mu, uint32_t now) {
  // res = 0 is "unknown", not empty: it appears while the reservoir is detached during a set
  // change. Drop the sample and keep the baseline, so the refill's rise is what gets noticed.
  if (res_mu <= 0) return false;
  if (!a->have_res) {
    a->last_res_mu = res_mu;
    a->have_res = true;
    a->last_sample_ts = now;
    a->first_sample_ts = now;
    return false;
  }
  const int32_t previous = a->last_res_mu;
  a->last_res_mu = res_mu;

  const uint32_t gap_secs = now - a->last_sample_ts;
  a->last_sample_ts = now;
  if (res_mu > previous) {
    // A refill or prime: insulin went into the reservoir, not into the body.
    a->prime_until = now + MINIMED_ACTIVITY_PRIME_SECS;
    return false;
  }

  const int32_t delta = previous - res_mu;
  if (delta <= 0) return false;
  if (now < a->prime_until) return false;  // priming burst still in progress

  if (gap_secs > MINIMED_ACTIVITY_GAP_SECS) {
    // No samples across the span, so the drop cannot be timed: spread it as a flat rate and mark
    // the curve provisional until the gap is older than the kernel. The ledger is capped at
    // 256 cells, so a very long gap is spread over the most recent cells only.
    uint32_t cells = gap_secs / MINIMED_ACTIVITY_STEP_SECS;
    if (cells > MINIMED_ACTIVITY_LEDGER_MAX) cells = MINIMED_ACTIVITY_LEDGER_MAX;
    if (cells == 0) cells = 1;
    const uint32_t start = now - cells * MINIMED_ACTIVITY_STEP_SECS;
    const int32_t per_cell = delta / (int32_t)cells;
    const int32_t remainder = delta - per_cell * (int32_t)cells;
    for (uint32_t i = 0; i < cells; i++) {
      prv_record(a, start + i * MINIMED_ACTIVITY_STEP_SECS,
                 per_cell + (i == cells - 1 ? remainder : 0));
    }
    const uint32_t until = a->last_sample_ts - gap_secs + MINIMED_ACTIVITY_TD_MIN * 60;
    if (until > a->provisional_until) a->provisional_until = until;
    return true;
  }

  prv_record(a, now, delta);
  return true;
}

float minimed_activity_eval_mu_per_min(const MinimedActivity *a, uint32_t t) {
  const uint16_t n = MINIMED_ACTIVITY_TD_MIN / 5 + 1;
  int64_t sum = 0;  // units: mu * 1e-6/min
  for (uint16_t i = 0; i < a->count; i++) {
    const uint16_t idx =
        (uint16_t)((a->head + MINIMED_ACTIVITY_LEDGER_MAX - 1 - i) % MINIMED_ACTIVITY_LEDGER_MAX);
    const MinimedActivityEntry *e = &a->ledger[idx];
    const int64_t dt = (int64_t)t - (int64_t)e->ts;
    if (dt < 0 || dt >= MINIMED_ACTIVITY_TD_MIN * 60) continue;
    const uint32_t step = (uint32_t)dt / MINIMED_ACTIVITY_STEP_SECS;
    const uint32_t frac = (uint32_t)dt % MINIMED_ACTIVITY_STEP_SECS;
    uint32_t k = s_kernel[step];
    if (step + 1 < n && frac != 0) {
      k = (k * (MINIMED_ACTIVITY_STEP_SECS - frac) + s_kernel[step + 1] * frac) /
          MINIMED_ACTIVITY_STEP_SECS;
    }
    sum += (int64_t)e->mu * (int32_t)k;
  }
  return (float)sum / (float)KERNEL_SCALE;
}

bool minimed_activity_provisional(const MinimedActivity *a, uint32_t now) {
  if (!a->have_res) return true;
  if (now - a->first_sample_ts < MINIMED_ACTIVITY_PROVISIONAL_SECS) return true;
  return now < a->provisional_until;
}

uint16_t minimed_activity_serialize(const MinimedActivity *a, uint32_t now, uint8_t *out) {
  if (!a->have_res) return 0;
  const uint32_t ref =
      now - (uint32_t)MINIMED_ACTIVITY_BACK_POINTS * MINIMED_ACTIVITY_STEP_SECS;
  out[0] = (uint8_t)ref;
  out[1] = (uint8_t)(ref >> 8);
  out[2] = (uint8_t)(ref >> 16);
  out[3] = (uint8_t)(ref >> 24);
  out[4] = (uint8_t)MINIMED_ACTIVITY_POINTS;
  out[5] = (uint8_t)(MINIMED_ACTIVITY_POINTS >> 8);
  uint8_t *vals = out + 6 + 2 * MINIMED_ACTIVITY_POINTS;
  for (uint16_t i = 0; i < MINIMED_ACTIVITY_POINTS; i++) {
    const uint16_t off_min = (uint16_t)(i * 5);
    out[6 + 2 * i] = (uint8_t)off_min;
    out[7 + 2 * i] = (uint8_t)(off_min >> 8);
    // 0.05 U/h per LSB = 50 mU/h = 50/60 mU/min, so LSB = activity * 60/50 = activity * 6/5.
    const float act = minimed_activity_eval_mu_per_min(a, ref + i * MINIMED_ACTIVITY_STEP_SECS);
    int32_t v = (int32_t)(act * 1.2f + 0.5f);
    vals[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
  return 6 + 3 * MINIMED_ACTIVITY_POINTS;
}
