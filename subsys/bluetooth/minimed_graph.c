/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_graph.h"

#include <string.h>

static void prv_drop_oldest(MinimedGraph *graph, uint16_t n) {
  if (n == 0) {
    return;
  }
  graph->count -= n;
  memmove(graph->ts, graph->ts + n, graph->count * sizeof(graph->ts[0]));
  memmove(graph->bg, graph->bg + n, graph->count * sizeof(graph->bg[0]));
}

void minimed_graph_add(MinimedGraph *graph, uint32_t timestamp, int32_t mgdl) {
  if (mgdl < 0) {
    return;
  }
  int32_t half = (mgdl + 1) / 2;
  if (half > 255) {
    half = 255;  // 510 mg/dL, above the sensor's range
  }

  // Keep the array strictly ascending. Only reachable if the RTC moved backwards (time sync, DST),
  // in which case the affected points are the newest ones.
  while (graph->count > 0 && graph->ts[graph->count - 1] >= timestamp) {
    graph->count--;
  }

  uint16_t drop = 0;
  while (drop < graph->count && graph->ts[drop] + MINIMED_GRAPH_RETENTION_SECS <= timestamp) {
    drop++;
  }
  if (graph->count - drop == MINIMED_GRAPH_MAX_POINTS) {
    drop++;  // full: make room for the new point
  }
  prv_drop_oldest(graph, drop);

  graph->ts[graph->count] = timestamp;
  graph->bg[graph->count] = (uint8_t)half;
  graph->count++;
}

uint16_t minimed_graph_serialize(const MinimedGraph *graph, uint32_t window_secs,
                                 uint8_t *out) {
  // Snapshot count once. The firmware appends on the BT host task and serializes on KernelMain
  // without a lock (the accepted convention here), so re-reading it could emit a blob whose
  // embedded count, byte layout and returned length disagree -- a garbled frame rather than a torn
  // value. One local keeps the blob self-consistent whatever the array does underneath.
  const uint16_t stored_count = graph->count;
  if (stored_count == 0 || window_secs == 0) {
    return 0;
  }

  uint32_t span = window_secs;
  if (span <= UINT32_MAX - MINIMED_GRAPH_MARGIN_SECS) {
    span += MINIMED_GRAPH_MARGIN_SECS;
  } else {
    span = UINT32_MAX;
  }
  const uint32_t newest = graph->ts[stored_count - 1];
  const uint32_t cutoff = newest > span ? newest - span : 0;
  uint16_t first = 0;
  while (first < stored_count && graph->ts[first] < cutoff) {
    first++;
  }
  if (first == stored_count) {
    return 0;
  }

  const uint16_t count = stored_count - first;
  const uint32_t ref = graph->ts[first];
  out[0] = (uint8_t)ref;
  out[1] = (uint8_t)(ref >> 8);
  out[2] = (uint8_t)(ref >> 16);
  out[3] = (uint8_t)(ref >> 24);
  out[4] = (uint8_t)(count);
  out[5] = (uint8_t)(count >> 8);

  uint8_t *bg = out + 6 + 2 * count;
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t source = first + i;
    // Offsets are minutes from the oldest point; the window bounds this well inside a u16.
    const uint16_t off_min = (uint16_t)((graph->ts[source] - ref) / 60);
    out[6 + 2 * i] = (uint8_t)off_min;
    out[7 + 2 * i] = (uint8_t)(off_min >> 8);
    bg[i] = graph->bg[source];
  }
  return (uint16_t)(6 + 3 * count);
}
