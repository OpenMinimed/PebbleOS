/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_basal_iob.h"

#include <string.h>

#define CELLS     MINIMED_BASAL_IOB_CELLS
#define CELL_SECS MINIMED_BASAL_IOB_CELL_SECS

void minimed_basal_iob_reset(MinimedBasalIobState *st) {
  memset(st, 0, sizeof(*st));
}

static int prv_slot(int32_t cell) {
  const int m = (int)(cell % CELLS);
  return m < 0 ? m + CELLS : m;
}

// Cell index of a timestamp, snapped to the nearest 5-minute mark like the predictor's grid.
static int32_t prv_cell_of(uint32_t ts) {
  return (int32_t)((ts + CELL_SECS / 2) / CELL_SECS);
}

void minimed_basal_iob_add_micro(MinimedBasalIobState *st, uint32_t ts, float units) {
  if (units <= 0.0f)
    return;
  const int32_t cell = prv_cell_of(ts);
  if (!st->have || cell - st->head >= CELLS) {
    memset(st->micro, 0, sizeof(st->micro));
    st->have = true;
    st->head = cell;
  } else if (cell > st->head) {
    for (int32_t c = st->head + 1; c <= cell; c++)
      st->micro[prv_slot(c)] = 0.0f;
    st->head = cell;
  } else if (cell <= st->head - CELLS) {
    return; // older than the ring holds, and so already spent
  }
  st->micro[prv_slot(cell)] += units;
}

void minimed_basal_iob_set_basal(MinimedBasalIobState *st, uint32_t ts, float u_per_h) {
  st->basal_u_per_h = u_per_h > 0.0f ? u_per_h : 0.0f;
  st->basal_from = prv_cell_of(ts);
}

int32_t minimed_basal_iob_mu(const MinimedBasalIobState *st, uint32_t now) {
  float iob = 0.0f;
  for (int32_t c = prv_cell_of(now);; c--) {
    const int64_t age = (int64_t)now - (int64_t)c * CELL_SECS;
    if (age >= MINIMED_IOB_AIT_SECS)
      break; // older cells are all spent
    float delivered = 0.0f;
    if (st->have && c <= st->head && c > st->head - CELLS)
      delivered = st->micro[prv_slot(c)];
    if (st->basal_u_per_h > 0.0f && c >= st->basal_from) {
      // The cell spans age +-150 s; the part after `now` has not been delivered yet.
      int64_t elapsed = age + CELL_SECS / 2;
      if (elapsed > CELL_SECS)
        elapsed = CELL_SECS;
      if (elapsed > 0)
        delivered += st->basal_u_per_h * (float)elapsed / 3600.0f;
    }
    const float left = 1.0f - (float)(age > 0 ? age : 0) / (float)MINIMED_IOB_AIT_SECS;
    iob += delivered * left;
  }
  return (int32_t)(iob * 1000.0f + 0.5f);
}
