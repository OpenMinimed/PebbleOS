/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_predict.h"

#include "minimed_predict_model.h"

#include <math.h>
#include <string.h>

#define WINDOW     MINIMED_PREDICT_WINDOW
#define CELLS      MINIMED_PREDICT_CELLS
#define STALE_SECS (10 * 60)

// -- Model (ported from sugar_predictor/firmware/cheap.c) -----------------------------------------

#if MINIMED_PREDICT_CGM_CELLS != CH_N
#error "model header and input struct disagree"
#endif

float minimed_predict_cheap(MinimedPredictCheapIn *in) {
  const float *b = in->bg;
  const float now = b[CH_N - 1];
  const float d5 = now - b[CH_N - 2];
  const float d10 = now - b[CH_N - 3];
  const float d30 = now - b[CH_N - 7];
  float mean = 0.0f, var = 0.0f;
  for (int i = CH_N - 12; i < CH_N; i++)
    mean += b[i];
  mean /= 12.0f;
  for (int i = CH_N - 12; i < CH_N; i++)
    var += (b[i] - mean) * (b[i] - mean);

  in->ema[0] = in->ema[0] * ch_ema_decay[0] + in->ins_prev;
  in->ema[1] = in->ema[1] * ch_ema_decay[1] + in->ins_prev;
  in->ema[2] = in->ema[2] * ch_ema_decay[2] + in->carb_prev;
  in->ema[3] = in->ema[3] * ch_ema_decay[3] + in->carb_prev;

  float t[CH_D];
  t[0] = d5;
  t[1] = d10;
  t[2] = now - b[CH_N - 4];
  t[3] = d30;
  t[4] = now - b[CH_N - 13];
  t[5] = d5 - (b[CH_N - 2] - b[CH_N - 3]);
  t[6] = now - 120.0f;
  t[7] = now < 70.0f ? 70.0f - now : 0.0f;
  t[8] = now > 180.0f ? now - 180.0f : 0.0f;
  t[9] = d5 < 0.0f ? -d5 : d5;
  t[10] = d5 * d10 / 10.0f;
  t[11] = (now - 120.0f) * d5 / 100.0f;
  t[12] = (now - 120.0f) * d30 / 100.0f;
  t[13] = sqrtf(var / 12.0f);
  t[14] = in->tod_sin;
  t[15] = in->tod_cos;
  t[16] = in->ema[0];
  t[17] = in->ema[1];
  t[18] = in->ema[2];
  t[19] = in->ema[3];

  float out = ch_b2;
  for (int j = 0; j < CH_H; j++) {
    float acc = 0.0f;
    for (int i = 0; i < CH_D; i++) {
      acc += (float)ch_w1[j * CH_D + i] * ((t[i] - ch_mu[i]) * ch_isd[i]);
    }
    acc = acc * ch_s_w1 + (float)ch_b1[j] * ch_s_b1;
    if (acc > 0.0f)
      out += acc * (float)ch_w2[j] * ch_s_w2;
  }
  return out * ch_ys + ch_ym + now;
}

// One 5-minute step of the net's decaying sums: what was delivered in a cell, as of the origin
// that ends it.
static void prv_ema_step(float ema[4], float ins, float carb) {
  ema[0] = ema[0] * ch_ema_decay[0] + ins;
  ema[1] = ema[1] * ch_ema_decay[1] + ins;
  ema[2] = ema[2] * ch_ema_decay[2] + carb;
  ema[3] = ema[3] * ch_ema_decay[3] + carb;
}

// Working buffer is static: the hypo model reads the window after minimed_predict_run returns.
// Nothing here is reentrant; the one caller is the MiniMed task.
static MinimedPredictWindow s_window;

void minimed_predict_window(const MinimedPredictWindow *in, MinimedPrediction *out) {
  MinimedPredictCheapIn c;
  memcpy(c.bg, in->bg + WINDOW - MINIMED_PREDICT_CGM_CELLS, sizeof(c.bg));
  c.tod_sin = in->tod_sin;
  c.tod_cos = in->tod_cos;
  memcpy(c.ema, in->ema0, sizeof(c.ema));
  // The model sums the cells that ended before the newest origin; it adds the last one itself.
  for (int k = 0; k < WINDOW - 2; k++)
    prv_ema_step(c.ema, in->ins[k], in->carb[k]);
  c.ins_prev = in->ins[WINDOW - 2];
  c.carb_prev = in->carb[WINDOW - 2];
  out->mgdl = minimed_predict_cheap(&c);
}

const MinimedPredictWindow *minimed_predict_last_window(void) {
  return &s_window;
}

// -- State ----------------------------------------------------------------------------------------

void minimed_predict_reset(MinimedPredictState *st) {
  memset(st, 0, sizeof(*st));
}

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

// What was delivered in a cell: logged insulin and carbs, plus the manual-mode basal carried
// forward. A cell the ring no longer holds has only the basal.
static void prv_cell_flow(const MinimedPredictState *st, int32_t cell, float *ins, float *carb) {
  *ins = 0.0f;
  *carb = 0.0f;
  if (cell <= st->head && cell > st->head - CELLS) {
    *ins = st->ins[prv_slot(cell)];
    *carb = st->carb[prv_slot(cell)];
  }
  if (st->basal_u_per_h > 0.0f && cell >= st->basal_from) {
    *ins += st->basal_u_per_h * (MINIMED_PREDICT_CELL_SECS / 3600.0f);
  }
}

// Fold a cell's flow into the net's settled sums. Called right before the ring evicts that cell
// for good, so this is the only chance to keep its contribution to iob180/cob120 (cheap.h: up to a
// 180-minute time constant) once it is gone. Idempotent: a cell at or before ema_cell is skipped,
// so replaying the same boundary twice (or settling a cell that was never actually written, whose
// flow reads as zero) does nothing.
static void prv_settle_slot(MinimedPredictState *st, int32_t cell) {
  if (cell > st->ema_cell) {
    float ins, carb;
    prv_cell_flow(st, cell, &ins, &carb);
    prv_ema_step(st->ema, ins, carb);
    st->ema_cell = cell;
  }
}

// Cell index of a timestamp, snapped to the nearest 5-minute mark like the training grid.
static int32_t prv_cell_of(uint32_t ts) {
  return (int32_t)((ts + MINIMED_PREDICT_CELL_SECS / 2) / MINIMED_PREDICT_CELL_SECS);
}

// The slot for a cell, advancing the ring if the cell is newer than any so far. NULL-equivalent
// (-1) for a cell that has already aged out of the ring.
static int prv_touch(MinimedPredictState *st, int32_t cell) {
  if (!st->have) {
    st->have = true;
    st->head = cell;
    st->ema_cell = cell - CELLS; // nothing settled before the ring's own start
    prv_clear_slot(st, cell);
  } else if (cell > st->head) {
    if (cell - st->head >= CELLS) {
      // The whole ring turns over in one jump: settle whatever was still live first (a real
      // gap, not a cell to throw away), then there is no data at all for the gap itself, so
      // jump the settled boundary straight to where the new ring starts.
      for (int32_t c = st->head - CELLS + 1; c <= st->head; c++)
        prv_settle_slot(st, c);
      st->ema_cell = cell - CELLS;
      for (int32_t c = cell - CELLS + 1; c <= cell; c++)
        prv_clear_slot(st, c);
    } else {
      for (int32_t c = st->head + 1; c <= cell; c++) {
        prv_settle_slot(st, c - CELLS); // read before prv_clear_slot overwrites the same slot
        prv_clear_slot(st, c);
      }
    }
    st->head = cell;
  } else if (cell <= st->head - CELLS) {
    return -1;
  }
  return prv_slot(cell);
}

void minimed_predict_add_bg(MinimedPredictState *st, uint32_t ts, int32_t mgdl) {
  if (mgdl <= 0)
    return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s < 0)
    return;
  st->bg[s] = (float)mgdl;
  st->bg_set[s] = true;
}

void minimed_predict_add_insulin(MinimedPredictState *st, uint32_t ts, float units) {
  if (units <= 0.0f)
    return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s >= 0)
    st->ins[s] += units;
}

void minimed_predict_add_carbs(MinimedPredictState *st, uint32_t ts, float grams) {
  if (grams <= 0.0f)
    return;
  const int s = prv_touch(st, prv_cell_of(ts));
  if (s >= 0)
    st->carb[s] += grams;
}

void minimed_predict_set_basal(MinimedPredictState *st, uint32_t ts, float u_per_h) {
  st->basal_u_per_h = u_per_h > 0.0f ? u_per_h : 0.0f;
  st->basal_from = prv_cell_of(ts);
}

// -- Run ------------------------------------------------------------------------------------------

static float prv_cell_bg(const MinimedPredictState *st, int32_t cell, bool *set) {
  *set = false;
  if (cell > st->head || cell <= st->head - CELLS)
    return 0.0f;
  const int s = prv_slot(cell);
  *set = st->bg_set[s];
  return st->bg[s];
}

bool minimed_predict_run(const MinimedPredictState *st, uint32_t now, int32_t gmt_offset_secs,
                         MinimedPrediction *out) {
  if (!st->have)
    return false;

  // The newest cell that holds a reading.
  int32_t newest = st->head;
  bool set = false;
  while (newest > st->head - CELLS) {
    prv_cell_bg(st, newest, &set);
    if (set)
      break;
    newest--;
  }
  if (!set)
    return false;
  if ((int64_t)now - (int64_t)newest * MINIMED_PREDICT_CELL_SECS >
      STALE_SECS + MINIMED_PREDICT_CELL_SECS / 2) {
    return false;
  }

  MinimedPredictWindow *inp = &s_window;
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
    inp->bg[k] = have_last ? last : 0.0f;
  }
  int lead = 0;
  while (lead < WINDOW && inp->bg[lead] == 0.0f)
    lead++;
  for (int k = 0; k < lead && lead < WINDOW; k++)
    inp->bg[k] = inp->bg[lead];

  for (int k = 0; k < WINDOW; k++)
    prv_cell_flow(st, first + k, &inp->ins[k], &inp->carb[k]);

  // The net's insulin and carb sums want up to 15 h to settle (cheap.h) and the window only spans
  // 4: start from MinimedPredictState's settled ema (everything that has ever aged out of the
  // ring, not just zero) and live-replay the still-editable ring cells between it and the window.
  memcpy(inp->ema0, st->ema, sizeof(inp->ema0));
  for (int32_t c = st->ema_cell + 1; c < first; c++) {
    float i, g;
    prv_cell_flow(st, c, &i, &g);
    prv_ema_step(inp->ema0, i, g);
  }

  // Local time of day.
  const int64_t local = (int64_t)now + gmt_offset_secs;
  const int64_t days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
  const int64_t sec_of_day = local - days * 86400;
  const float minutes = (float)sec_of_day / 60.0f;
  const float two_pi = 6.28318530717958647692f;
  inp->tod_sin = sinf(two_pi * minutes / 1440.0f);
  inp->tod_cos = cosf(two_pi * minutes / 1440.0f);
  inp->hour = (int)(sec_of_day / 3600);

  minimed_predict_window(inp, out);

  int cells = 0;
  float ins = 0.0f, carb = 0.0f;
  for (int k = 0; k < WINDOW; k++) {
    bool s;
    prv_cell_bg(st, first + k, &s);
    if (s)
      cells++;
    if (k < WINDOW - 1) { // the newest cell is ignored by the model
      ins += inp->ins[k];
      carb += inp->carb[k];
    }
  }
  out->bg_cells = (uint8_t)cells;
  out->ins_cu = (int16_t)(ins * 100.0f + 0.5f);
  out->carb_g = (int16_t)(carb + 0.5f);
  out->slope_x10 = (int16_t)((inp->bg[WINDOW - 1] - inp->bg[WINDOW - 4]) * 10.0f / 15.0f);
  out->hour = (uint8_t)inp->hour;
  return true;
}

void minimed_predict_score_reset(MinimedPredictScore *sc) {
  memset(sc, 0, sizeof(*sc));
}

void minimed_predict_score_note(MinimedPredictScore *sc, uint32_t ts, int32_t pred_mgdl,
                                int32_t base_mgdl) {
  if (sc->pending == MINIMED_SCORE_PENDING) { // drop the oldest
    memmove(&sc->due[0], &sc->due[1], (MINIMED_SCORE_PENDING - 1) * sizeof(sc->due[0]));
    memmove(&sc->pred[0], &sc->pred[1], (MINIMED_SCORE_PENDING - 1) * sizeof(sc->pred[0]));
    memmove(&sc->base[0], &sc->base[1], (MINIMED_SCORE_PENDING - 1) * sizeof(sc->base[0]));
    sc->pending--;
  }
  sc->due[sc->pending] = ts + MINIMED_SCORE_HORIZON_SECS;
  sc->pred[sc->pending] = pred_mgdl;
  sc->base[sc->pending] = base_mgdl;
  sc->pending++;
}

bool minimed_predict_score_actual(MinimedPredictScore *sc, uint32_t ts, int32_t mgdl,
                                  int32_t *err) {
  bool scored = false;
  uint8_t keep = 0;
  for (uint8_t i = 0; i < sc->pending; i++) {
    const int64_t d = (int64_t)ts - (int64_t)sc->due[i];
    if (d > MINIMED_SCORE_TOLERANCE_SECS)
      continue; // missed its reading: forget it
    if (d >= -MINIMED_SCORE_TOLERANCE_SECS && !scored) {
      const int32_t e = sc->pred[i] - mgdl;
      const int32_t eb = sc->base[i] - mgdl;
      sc->sse += (uint64_t)((int64_t)e * e);
      sc->sse_base += (uint64_t)((int64_t)eb * eb);
      sc->count++;
      if (err)
        *err = e;
      scored = true;
      continue;
    }
    sc->due[keep] = sc->due[i];
    sc->pred[keep] = sc->pred[i];
    sc->base[keep] = sc->base[i];
    keep++;
  }
  sc->pending = keep;
  return scored;
}

uint32_t minimed_predict_score_rmse_x10(const MinimedPredictScore *sc, bool baseline) {
  if (sc->count == 0)
    return 0;
  const float mse = (float)(baseline ? sc->sse_base : sc->sse) / (float)sc->count;
  return (uint32_t)(sqrtf(mse) * 10.0f + 0.5f);
}
