/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_hypo_check.h"

#include "drivers/rtc.h"
#include "minimed_hypo.h"
#include "minimed_sake_sender.h"
#include "minimed_settings.h"
#include "popups/minimed_alert_popup.h"
#include "popups/minimed_sake_ui.h"

#include <stdio.h>

#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// A falling low: score whether to treat, with the sugar_predictor/firmware/hypo.c model ported to
// minimed_hypo.c. Fires at a decision point (minimed_hypo_should_evaluate, about 16 times a day on
// the wearer this was fitted on), or earlier still when minimed_hypo_falling_fast says the reading
// is about to plunge into that regime faster than it can wait for -- see its own comment for the
// physiological rate this is anchored on.
// The hypo model's own popup, like a pump alarm's: once when the treat score first reaches
// HYPO_ALERT_TREAT_PCT, then not again until it has dropped below (or left the falling-low regime)
// AND the cooldown has passed, so a score hovering at the threshold cannot buzz every 5 minutes.
// The threshold is the watchface's default TREAT level (HYPO_TREAT_THRESHOLD_DEFAULT there, the
// model's validated cut-off); the phone-set threshold only reaches the watchface. Switched on and
// off by its own setting (MINIMED_ALERT_HYPO_MODEL, "Alert" in the MiniMed app's Hypo model menu).
#define HYPO_ALERT_TREAT_PCT     32
#define HYPO_ALERT_COOLDOWN_SECS (30 * 60)
static bool s_hypo_alert_armed = true;
static uint32_t s_hypo_alert_last;

// mg/dL -> "N.N" mmol/L, rounded the way the pump displays it (see minimed_session.c).
static void prv_mmol_str(char *out, size_t n, float mgdl) {
  const int32_t m = (int32_t)(mgdl + 0.5f);
  const int32_t tenths = (m * 100000 + 90091) / 180182;
  snprintf(out, n, "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
}

static void prv_hypo_alert(const MinimedHypoPrediction *h, int32_t treat_pct, bool early,
                           const char *bg_str) {
  const uint32_t now = (uint32_t)rtc_get_time();
  if (treat_pct < HYPO_ALERT_TREAT_PCT) {
    s_hypo_alert_armed = true;
    return;
  }
  if (!s_hypo_alert_armed ||
      (s_hypo_alert_last != 0 && now - s_hypo_alert_last < HYPO_ALERT_COOLDOWN_SECS) ||
      !minimed_settings_hypo_alert_enabled()) {
    return;
  }
  s_hypo_alert_armed = false;
  s_hypo_alert_last = now;
  char nadir[16], nadir_treated[16];
  prv_mmol_str(nadir, sizeof(nadir), h->nadir_untreated);
  prv_mmol_str(nadir_treated, sizeof(nadir_treated), h->nadir_treated);
  char body[160];
  snprintf(body, sizeof(body),
           "%sTreat %ld%%, low chance %d%% (severe %d%%)\n"
           "Lowest %s in the next hour, %s if treated\n"
           "~%ld min below 3.9\nNow %s",
           early ? "Falling fast. " : "", (long)treat_pct, (int)(h->p_low * 100.0f + 0.5f),
           (int)(h->p_severe * 100.0f + 0.5f), nadir, nadir_treated,
           (long)(h->mins_untreated + 0.5f), bg_str[0] != '\0' ? bg_str : "---");
  minimed_sake_log("hypo alert (model)");
  PBL_LOG_INFO("minimed: hypo alert shown: treat=%ld pct%s", (long)treat_pct,
               early ? " (early fast-fall)" : "");
  minimed_alert_popup_push("Low predicted (model)", body,
                           minimed_settings_hypo_dnd_bypass_enabled());
}

void minimed_hypo_check(const MinimedPredictWindow *win, const char *bg_str) {
  if (!minimed_settings_hypo_enabled()) {
    return; // phone-disabled: skip the computation entirely, not just the send/display
  }
  const bool normal = win && minimed_hypo_should_evaluate(win);
  const bool early = win && !normal && minimed_hypo_falling_fast(win);
  if (!normal && !early) {
    minimed_sake_sender_send_hypo(false, 0, 0); // out of the falling-low regime: clear any banner
    s_hypo_alert_armed = true;
    return;
  }
  MinimedHypoPrediction h;
  const RtcTicks hypo_start = rtc_get_ticks();
  minimed_hypo_eval(win, 0.3f, &h);
  const uint32_t hypo_ms = (uint32_t)(((rtc_get_ticks() - hypo_start) * 1000) / RTC_TICKS_HZ);
  PBL_LOG_INFO(
      "minimed: hypo%s treat=%d pct, nadir %ld/%ld mg/dL (untreated/treated), mins<70 "
      "%ld/%ld, %lums",
      early ? " (early fast-fall)" : "", (int)(h.treat_pct + 0.5f),
      (long)(h.nadir_untreated + 0.5f), (long)(h.nadir_treated + 0.5f),
      (long)(h.mins_untreated + 0.5f), (long)(h.mins_treated + 0.5f), (unsigned long)hypo_ms);
  PBL_LOG_INFO("minimed: hypo p_low=%d pct p_severe=%d pct p_over=%d pct",
               (int)(h.p_low * 100.0f + 0.5f), (int)(h.p_severe * 100.0f + 0.5f),
               (int)(h.p_over * 100.0f + 0.5f));
  char line[64];
  snprintf(line, sizeof(line), "hypo%s t%d n%ld/%ld m%ld/%ld", early ? "!" : "",
           (int)(h.treat_pct + 0.5f), (long)(h.nadir_untreated + 0.5f),
           (long)(h.nadir_treated + 0.5f), (long)(h.mins_untreated + 0.5f),
           (long)(h.mins_treated + 0.5f));
  minimed_sake_log(line);
  const int32_t pct = (int32_t)(h.treat_pct + 0.5f);
  const int32_t p_low_pct = (int32_t)(h.p_low * 100.0f + 0.5f);
  prv_hypo_alert(&h, pct, early, bg_str);
  minimed_sake_sender_send_hypo(true, (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct)),
                                (uint8_t)(p_low_pct < 0 ? 0 : (p_low_pct > 100 ? 100 : p_low_pct)));
}
