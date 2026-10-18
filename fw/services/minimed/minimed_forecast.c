/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

// The 30-minute glucose forecast: feeds the predictor (core/minimed_predict.c) from the pump
// session's readings and history, scores it, and sends the result to the watchface. The hypo
// model reads the predictor's window, so it runs from here too.

#include "minimed_forecast.h"

#include "drivers/rtc.h"
#include "minimed_hypo_check.h"
#include "minimed_predict.h"
#include "minimed_sake_sender.h"
#include "minimed_settings.h"
#include "popups/minimed_sake_ui.h"

#include <stdio.h>

#include <pbl/logging/logging.h>
#include <pbl/util/time.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// The glucose predictor's view of the last 4 hours. Filled from the backfill (which resets it, so
// a reconnect cannot count an event twice) and then live; it predicts only once the backfill has
// given it the insulin and meals of the window, or it would see a body with no history.
static MinimedPredictState s_pred;
static MinimedPredictScore s_pred_score;
static bool s_pred_ready;

// Local-time offset of the watch, seconds east of UTC.
static int32_t prv_gmt_offset(uint32_t now) {
  return (int32_t)(time_utc_to_local((time_t)now) - (time_t)now);
}

// Switched off in the menu: the predictor's state is still fed, so it forecasts again as soon as
// it is switched back on.
static bool prv_enabled(void) {
  return minimed_settings_feature_enabled(MINIMED_FEATURE_PREDICTOR);
}

// Predict 30 minutes ahead from the newest reading and hand the result to the watchface. A reading
// that cannot be predicted (switched off, stale, or the predictor not primed) clears the last
// prediction.
static void prv_predict_and_send(int32_t mgdl, const char *bg_str) {
  MinimedPrediction p;
  const uint32_t now = (uint32_t)rtc_get_time();
  const RtcTicks predict_start = rtc_get_ticks();
  const bool have_pred =
      prv_enabled() && s_pred_ready && minimed_predict_run(&s_pred, now, prv_gmt_offset(now), &p);
  const uint32_t predict_ms = (uint32_t)(((rtc_get_ticks() - predict_start) * 1000) / RTC_TICKS_HZ);
  if (have_pred) {
    const int32_t pred = (int32_t)(p.mgdl + 0.5f);
    PBL_LOG_INFO("minimed: predict %ld mg/dL in 30 min (now %ld, %+ld), %lums", (long)pred,
                 (long)mgdl, (long)(pred - mgdl), (unsigned long)predict_ms);
    PBL_LOG_INFO(
        "minimed: predict inputs bg=%u/48 cells, slope15=%+d.%d mg/dL/min, ins4h=%d.%02dU, "
        "carb4h=%dg, hour=%u",
        p.bg_cells, p.slope_x10 / 10, (p.slope_x10 < 0 ? -p.slope_x10 : p.slope_x10) % 10,
        p.ins_cu / 100, p.ins_cu % 100, p.carb_g, p.hour);
    // The watch's log view keeps 31 characters after the time; the rest is cut there.
    char line[64];
    snprintf(line, sizeof(line), "pred %ld %+ld", (long)pred, (long)(pred - mgdl));
    minimed_sake_log(line);
    snprintf(line, sizeof(line), "in bg%u s%+d.%d i%d.%dU c%dg h%u", p.bg_cells, p.slope_x10 / 10,
             (p.slope_x10 < 0 ? -p.slope_x10 : p.slope_x10) % 10, p.ins_cu / 100,
             (p.ins_cu % 100) / 10, p.carb_g, p.hour);
    minimed_sake_log(line);
    minimed_predict_score_note(&s_pred_score, now, pred, mgdl);
    minimed_sake_sender_send_prediction(true, pred);
    minimed_hypo_check(minimed_predict_last_window(), bg_str);
  } else {
    minimed_sake_sender_send_prediction(false, 0);
  }
}

// Score the forecast that came due with this reading.
static void prv_score(uint32_t ts, int32_t mgdl) {
  int32_t err = 0;
  const bool scored = minimed_predict_score_actual(&s_pred_score, ts, mgdl, &err);
  const uint32_t r = minimed_predict_score_rmse_x10(&s_pred_score, false);
  const uint32_t b = minimed_predict_score_rmse_x10(&s_pred_score, true);
  if (scored) {
    PBL_LOG_INFO(
        "minimed: predict score n=%lu err=%+ld rmse=%lu.%lu carry-forward=%lu.%lu mg/dL "
        "since start",
        (unsigned long)s_pred_score.count, (long)err, (unsigned long)(r / 10),
        (unsigned long)(r % 10), (unsigned long)(b / 10), (unsigned long)(b % 10));
  }
  // Always show the score in the watch's log view, also before the first forecast is due.
  char line[64];
  if (s_pred_score.count == 0) {
    const uint32_t first_due = s_pred_score.pending ? s_pred_score.due[0] : 0;
    const uint32_t wait_min = first_due > ts ? (first_due - ts + 59) / 60 : 0;
    snprintf(line, sizeof(line), "rmse n0, first in %lum", (unsigned long)wait_min);
  } else {
    snprintf(line, sizeof(line), "rmse%lu.%lu cf%lu.%lu n%lu err%+ld", (unsigned long)(r / 10),
             (unsigned long)(r % 10), (unsigned long)(b / 10), (unsigned long)(b % 10),
             (unsigned long)s_pred_score.count, scored ? (long)err : 0L);
  }
  minimed_sake_log(line);
  PBL_LOG_INFO("minimed: predict %s", line);
}

// A live reading (not a backfilled one): score the forecast that came due, feed the predictor
// and refresh the prediction.
void minimed_forecast_reading(uint32_t ts, int32_t mgdl, const char *bg_str) {
  if (prv_enabled())
    prv_score(ts, mgdl);
  minimed_predict_add_bg(&s_pred, ts, mgdl);
  prv_predict_and_send(mgdl, bg_str);
}

// A live or backfilled insulin, meal or basal record.
void minimed_forecast_event(uint32_t ts, MinimedHistEventKind kind, float value) {
  if (kind == MinimedHistEventInsulin || kind == MinimedHistEventMicro) {
    minimed_predict_add_insulin(&s_pred, ts, value);
  } else if (kind == MinimedHistEventCarbs) {
    minimed_predict_add_carbs(&s_pred, ts, value);
  } else if (kind == MinimedHistEventBasal) {
    minimed_predict_set_basal(&s_pred, ts, value);
  }
}

void minimed_forecast_backfill_begin(void) {
  minimed_predict_reset(&s_pred);
}

void minimed_forecast_backfill_bg(uint32_t ts, int32_t mgdl) {
  minimed_predict_add_bg(&s_pred, ts, mgdl);
}

void minimed_forecast_backfill_end(bool primed, int32_t mgdl, const char *bg_str) {
  s_pred_ready = primed;
  prv_predict_and_send(mgdl, bg_str);
}

void minimed_forecast_unprime(void) {
  s_pred_ready = false;
}
