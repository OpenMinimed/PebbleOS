/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Insulin on board from basal delivery: the Auto Basal microboluses, and in manual mode the
//! programmed basal rate. The pump's own IOB counts boluses only, so the total is the pump's
//! number plus this one. Each delivered unit counts in full when given and falls linearly to
//! nothing over MINIMED_IOB_AIT_SECS. Pure, like the rest of core/.

#define MINIMED_IOB_AIT_SECS        (2 * 3600)
#define MINIMED_BASAL_IOB_CELL_SECS 300
//! Enough 5-minute cells to cover the active time, plus the half cells at either end.
#define MINIMED_BASAL_IOB_CELLS (MINIMED_IOB_AIT_SECS / MINIMED_BASAL_IOB_CELL_SECS + 2)

typedef struct {
  int32_t head; //!< absolute index (unix time / 300) of the newest microbolus cell; valid if have
  bool have;
  float micro[MINIMED_BASAL_IOB_CELLS]; //!< U of Auto Basal microboluses in the cell
  float basal_u_per_h;                  //!< manual-mode basal carried forward, 0 in auto mode
  int32_t basal_from;                   //!< first cell the carried rate applies to
} MinimedBasalIobState;

void minimed_basal_iob_reset(MinimedBasalIobState *st);

//! An Auto Basal microbolus at `ts`, units.
void minimed_basal_iob_add_micro(MinimedBasalIobState *st, uint32_t ts, float units);

//! Manual-mode basal rate from `ts` on (U/h); 0 when the pump's algorithm delivers basal instead.
void minimed_basal_iob_set_basal(MinimedBasalIobState *st, uint32_t ts, float u_per_h);

//! Basal insulin on board at `now`, milliunits. Only meaningful once the last
//! MINIMED_IOB_AIT_SECS of history have been loaded.
int32_t minimed_basal_iob_mu(const MinimedBasalIobState *st, uint32_t now);
