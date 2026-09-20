/* Replays a pump's event log (export_history_db.py) through the firmware's history decoding and
 * glucose predictor, and scores the 30-minute predictions against the readings that followed.
 * The pump clock is used as if it were local time (its Reference Times carry no zone), which is
 * also what the model was trained on. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minimed_history.h"
#include "minimed_predict.h"

#define FLOOR_MGDL 50
#define CEILING_MGDL 400
#define MAX_PRED 40000

static int hexval(int c) {
  return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1);
}

typedef struct {
  uint32_t secs;
  float mgdl, now_bg, pred;
} Pred;

static Pred g_pred[MAX_PRED];
static int g_npred;

int main(int argc, char **argv) {
  double biob_sum = 0;
  int32_t biob_max = 0;
  FILE *f = fopen(argc > 1 ? argv[1] : "history.hex", "r");
  if (!f) { perror("history.hex"); return 2; }
  static MinimedPredictState st;
  MinimedHistClock clk = {0};
  minimed_predict_reset(&st);
  char line[512];
  long recs = 0, sgs = 0, ins = 0, carbs = 0, rates = 0;
  double insulin_total = 0;

  while (fgets(line, sizeof(line), f)) {
    uint8_t rec[128];
    int n = 0;
    for (char *p = line; hexval(p[0]) >= 0 && hexval(p[1]) >= 0 && n < (int)sizeof(rec); p += 2) {
      rec[n++] = (uint8_t)(hexval(p[0]) * 16 + hexval(p[1]));
    }
    recs++;
    MinimedHistEvent ev;
    if (!minimed_history_decode(&clk, rec, (uint16_t)n, FLOOR_MGDL, CEILING_MGDL, &ev)) continue;
    switch (ev.kind) {
      case MinimedHistEventRef: break;
      case MinimedHistEventMicro:
        minimed_predict_add_micro(&st, ev.secs, ev.value);
        insulin_total += ev.value;
        ins++;
        break;
      case MinimedHistEventInsulin:
        minimed_predict_add_insulin(&st, ev.secs, ev.value);
        insulin_total += ev.value;
        ins++;
        break;
      case MinimedHistEventBasal:
        minimed_predict_set_basal(&st, ev.secs, ev.value);
        rates++;
        break;
      case MinimedHistEventCarbs:
        minimed_predict_add_carbs(&st, ev.secs, ev.value);
        carbs++;
        break;
      case MinimedHistEventSg: {
        if (ev.mgdl < 0) break;
        sgs++;
        minimed_predict_add_bg(&st, ev.secs, ev.mgdl);
        const int32_t biob = minimed_predict_basal_iob_mu(&st, ev.secs);
        biob_sum += biob;
        if (biob > biob_max) biob_max = biob;
        MinimedPrediction p;
        if (g_npred < MAX_PRED && minimed_predict_run(&st, ev.secs, 0, &p)) {
          g_pred[g_npred++] = (Pred){ev.secs, 0.0f, (float)ev.mgdl, p.mgdl};
        }
        break;
      }
    }
  }
  fclose(f);

  /* Score: a prediction made at T against the sample nearest T+30 min (within a minute). */
  double se = 0, sp = 0, ae = 0;
  long n = 0;
  int j = 0;
  for (int i = 0; i < g_npred; i++) {
    const uint32_t want = g_pred[i].secs + 1800;
    while (j < g_npred && g_pred[j].secs + 60 < want) j++;
    if (j >= g_npred) break;
    if (g_pred[j].secs > want + 60 || g_pred[j].secs + 60 < want) continue;
    const double truth = g_pred[j].now_bg;
    const double e = g_pred[i].pred - truth, b = g_pred[i].now_bg - truth;
    se += e * e;
    sp += b * b;
    ae += fabs(e);
    n++;
  }
  printf("%ld records: %ld samples, %ld insulin events (%.1f U), %ld meals, %ld rate changes\n", recs,
         sgs, ins, insulin_total, carbs, rates);
  printf("basal IOB at each sample: mean %.2f U, max %.2f U\n",
         sgs ? biob_sum / (double)sgs / 1000.0 : 0.0, biob_max / 1000.0);
  printf("%ld scored predictions: model RMSE %.2f MAE %.2f, persistence RMSE %.2f mg/dL\n", n,
         sqrt(se / n), ae / n, sqrt(sp / n));
  return 0;
}
