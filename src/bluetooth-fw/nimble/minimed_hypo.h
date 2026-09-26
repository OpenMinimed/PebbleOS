/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include "minimed_predict.h"

//! Treat-or-wait scoring for a falling low (ported from sugar_predictor/firmware/hypo.c). Reuses
//! the same 4-hour window the 30-minute predictor already builds (MinimedPredictWindow) to forecast
//! the next hour with and without the usual rescue dose, and to score how strongly to treat. Only
//! meaningful at a falling low: minimed_hypo_should_evaluate gates that, and the model says nothing
//! useful anywhere else. Not dose-aware -- see sugar_predictor/INTEGRATION.md.

typedef struct {
  float nadir_untreated;  //!< mg/dL, lowest reading expected in the next hour, no treatment
  float nadir_treated;    //!< mg/dL, after a typical rescue dose (HYPO_DOSE_G, for reference only)
  float mins_untreated;   //!< minutes below 70 mg/dL expected in the next hour, no treatment
  float mins_treated;
  float mins_saved;  //!< mins_untreated - mins_treated
  float p_low;        //!< P(nadir < 70), no treatment
  float p_severe;      //!< P(nadir < 54), no treatment
  float p_over;        //!< P(peak > 180) after the dose
  float treat_pct;     //!< 0..100 recommendation: 100 * clip(p_low - w * p_over, 0, 1)
} MinimedHypoPrediction;

//! True when the newest reading in `in` is at or below 90 mg/dL and has fallen over the last 15
//! minutes -- the only regime the model was fitted in. minimed_hypo_eval's output is undefined
//! outside of it.
bool minimed_hypo_should_evaluate(const MinimedPredictWindow *in);

//! Score a falling low. `over_treat_weight` is the single policy knob (0.3 in the benchmark).
void minimed_hypo_eval(const MinimedPredictWindow *in, float over_treat_weight,
                       MinimedHypoPrediction *out);
