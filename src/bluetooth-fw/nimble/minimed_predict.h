/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! 30-minute-ahead glucose predictor: a 38-feature, 120-unit int8 network (about 6.4 KB) fitted on
//! one wearer's year of MiniMed 780G data (sugar_predictor/REPORT.md: 20.5 mg/dL test RMSE against
//! 24.4 for carrying the current value forward). Pure -- no NimBLE, no firmware dependencies -- so
//! the host harness in tools/minimed_sake_hosttest can run it.
//!
//! The model wants the last 4 hours on a 5-minute grid: CGM, insulin delivered per cell (basal and
//! bolus) and carbohydrates entered per cell. MinimedPredictState collects those from timestamped
//! events, in any order, and minimed_predict_run builds the model's input from it.

#define MINIMED_PREDICT_WINDOW 48   //!< cells the model reads (4 h)
#define MINIMED_PREDICT_CELLS 64    //!< ring size: the window plus room for late events
#define MINIMED_PREDICT_CELL_SECS 300

typedef struct {
  int32_t head;  //!< absolute index (unix time / 300) of the newest cell touched; valid if have
  bool have;
  float bg[MINIMED_PREDICT_CELLS];   //!< mg/dL
  bool bg_set[MINIMED_PREDICT_CELLS];
  float ins[MINIMED_PREDICT_CELLS];  //!< U delivered in the cell (boluses, microboluses)
  float carb[MINIMED_PREDICT_CELLS]; //!< g entered in the cell
  float basal_u_per_h;               //!< manual-mode basal carried forward, 0 in auto mode
  int32_t basal_from;                //!< first cell the carried rate applies to
} MinimedPredictState;

void minimed_predict_reset(MinimedPredictState *st);

//! Record a CGM reading. A cell keeps its last reading.
void minimed_predict_add_bg(MinimedPredictState *st, uint32_t ts, int32_t mgdl);
//! Insulin delivered at `ts` (a bolus, or one auto-basal microbolus), units.
void minimed_predict_add_insulin(MinimedPredictState *st, uint32_t ts, float units);
//! Carbohydrates entered at `ts`, grams.
void minimed_predict_add_carbs(MinimedPredictState *st, uint32_t ts, float grams);
//! Manual-mode basal rate from `ts` on (U/h); 0 when the pump's algorithm delivers basal instead.
void minimed_predict_set_basal(MinimedPredictState *st, uint32_t ts, float u_per_h);

//! The model's input as the training set defined it, oldest cell first, cell 47 the reading at t.
//! bg carries gaps forward. ins/carb cell 47 covers [t, t+5) and is ignored. `dow_*` counts days
//! from THURSDAY = 0 (days since 1970 mod 7). Public so the host harness can replay the recorded
//! test vectors; the firmware goes through minimed_predict_run.
typedef struct {
  float bg[MINIMED_PREDICT_WINDOW];
  float ins[MINIMED_PREDICT_WINDOW];
  float carb[MINIMED_PREDICT_WINDOW];
  float tod_sin, tod_cos;
  float dow_sin, dow_cos;
  int hour;
} MinimedPredictWindow;

typedef struct {
  float mgdl;      //!< predicted glucose 30 minutes after `now`, mg/dL
  float low_prob;  //!< probability of below 70 mg/dL 30 minutes ahead
  bool low_alarm;  //!< low_prob is at or above the alarm threshold
  // What the model saw, for the log. Set by minimed_predict_run only.
  uint8_t bg_cells;  //!< window cells holding a real reading (of 48)
  int16_t ins_cu;    //!< insulin over the window, hundredths of a unit, newest cell excluded
  int16_t carb_g;    //!< carbohydrates over the window, grams, newest cell excluded
  int16_t slope_x10; //!< change over the last 15 minutes, tenths of mg/dL per minute
  uint8_t hour;      //!< local hour the model was given
} MinimedPrediction;

//! Predict from the newest reading. `now` is the wall clock; `gmt_offset_secs` turns it into local
//! time, which the model's time-of-day and day-of-week features use. False when there is no
//! reading, or the newest is more than 10 minutes older than `now`.
bool minimed_predict_run(const MinimedPredictState *st, uint32_t now, int32_t gmt_offset_secs,
                         MinimedPrediction *out);

//! Predict from an assembled window.
void minimed_predict_window(const MinimedPredictWindow *in, MinimedPrediction *out);

//! Running accuracy of the 30-minute forecast since start: each forecast waits for the reading
//! that arrives 30 minutes later. The baseline carries the reading at forecast time forward.
#define MINIMED_SCORE_PENDING 8
#define MINIMED_SCORE_HORIZON_SECS (30 * 60)
#define MINIMED_SCORE_TOLERANCE_SECS 150

typedef struct {
  uint32_t due[MINIMED_SCORE_PENDING];  //!< when the forecast comes true
  int32_t pred[MINIMED_SCORE_PENDING];
  int32_t base[MINIMED_SCORE_PENDING];
  uint8_t pending;
  uint32_t count;
  uint64_t sse;       //!< sum of squared forecast errors, mg/dL^2
  uint64_t sse_base;  //!< same for the carry-forward baseline
} MinimedPredictScore;

void minimed_predict_score_reset(MinimedPredictScore *sc);
//! Remember a forecast made at `ts` from reading `base_mgdl`.
void minimed_predict_score_note(MinimedPredictScore *sc, uint32_t ts, int32_t pred_mgdl,
                                int32_t base_mgdl);
//! A live reading arrived. Scores the forecast due at `ts` (within the tolerance), drops older
//! ones. True and `*err` (forecast minus actual) when one was scored.
bool minimed_predict_score_actual(MinimedPredictScore *sc, uint32_t ts, int32_t mgdl,
                                  int32_t *err);
//! RMSE in tenths of mg/dL; 0 when nothing is scored yet.
uint32_t minimed_predict_score_rmse_x10(const MinimedPredictScore *sc, bool baseline);
