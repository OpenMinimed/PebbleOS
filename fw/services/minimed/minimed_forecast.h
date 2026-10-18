/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "minimed_history.h"

#include <stdbool.h>
#include <stdint.h>

//! The 30-minute glucose forecast and the hypo model that reads its window. The pump session feeds
//! it readings and history events; it sends its results to the watchface itself. `bg_str` is the
//! BG as shown on the watchface, for the hypo alert's body.

#ifdef CONFIG_MINIMED_PREDICTOR

//! A new live reading: score the forecast that came due, then forecast again.
void minimed_forecast_reading(uint32_t ts, int32_t mgdl, const char *bg_str);

//! An insulin, meal or basal record, live or from the backfill. Other kinds are ignored.
void minimed_forecast_event(uint32_t ts, MinimedHistEventKind kind, float value);

//! The backfill: start over, take its readings (and events through minimed_forecast_event), then
//! forecast. `primed` says whether the backfill gave it enough history to forecast at all.
void minimed_forecast_backfill_begin(void);
void minimed_forecast_backfill_bg(uint32_t ts, int32_t mgdl);
void minimed_forecast_backfill_end(bool primed, int32_t mgdl, const char *bg_str);

//! The pump link went down: no forecasts until the next backfill.
void minimed_forecast_unprime(void);

#else

static inline void minimed_forecast_reading(uint32_t ts, int32_t mgdl, const char *bg_str) {}
static inline void minimed_forecast_event(uint32_t ts, MinimedHistEventKind kind, float value) {}
static inline void minimed_forecast_backfill_begin(void) {}
static inline void minimed_forecast_backfill_bg(uint32_t ts, int32_t mgdl) {}
static inline void minimed_forecast_backfill_end(bool primed, int32_t mgdl, const char *bg_str) {}
static inline void minimed_forecast_unprime(void) {}

#endif
