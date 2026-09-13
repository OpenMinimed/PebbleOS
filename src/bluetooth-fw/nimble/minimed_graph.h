/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! The BG history the watchface plots, and its wire encoding. Pure (no NimBLE, no firmware
//! dependencies) so the host harness in tools/minimed_sake_hosttest can test it directly.
//!
//! There is no pump-side backfill: the watch simply remembers the readings it has seen since it
//! last connected. A cold start therefore shows an empty graph that fills in over the following
//! hours.


//! The protocol permits requests for up to 24 hours. At five-minute intervals, the derived point
//! count covers that history plus the 30-minute margin needed to draw the entering left edge.
#define MINIMED_GRAPH_MAX_HOURS 24
#define MINIMED_GRAPH_MARGIN_SECS (30 * 60)
#define MINIMED_GRAPH_INTERVAL_SECS (5 * 60)
#define MINIMED_GRAPH_RETENTION_SECS \
  (MINIMED_GRAPH_MAX_HOURS * 60 * 60 + MINIMED_GRAPH_MARGIN_SECS)
#define MINIMED_GRAPH_MAX_POINTS \
  ((MINIMED_GRAPH_RETENTION_SECS / MINIMED_GRAPH_INTERVAL_SECS) + 1)

//! Wire format (pebble-glucose-protocol/PROTOCOL.md, GRAPH_DATA key 30), little-endian:
//! [ref_ts u32][count u16][offset_min u16 xN][bg u8 xN], where bg is mg/dL / 2.
#define MINIMED_GRAPH_BLOB_MAX (6 + 3 * MINIMED_GRAPH_MAX_POINTS)

typedef struct {
  uint32_t ts[MINIMED_GRAPH_MAX_POINTS];  //!< oldest first, strictly ascending
  uint8_t bg[MINIMED_GRAPH_MAX_POINTS];   //!< mg/dL / 2
  uint16_t count;
} MinimedGraph;

//! Append one reading, dropping points that have aged out of the window (and the oldest point if
//! the buffer is full). A negative `mgdl` is ignored. If the clock moved backwards, points that
//! are now in the future are discarded: the offset-from-oldest encoding cannot represent an
//! out-of-order point.
void minimed_graph_add(MinimedGraph *graph, uint32_t timestamp, int32_t mgdl);

//! Serialize the requested history plus MINIMED_GRAPH_MARGIN_SECS to `out` (at least
//! MINIMED_GRAPH_BLOB_MAX bytes). A zero window returns 0. Returns the byte count, or 0 when
//! there is nothing to plot -- callers should then omit the field rather than send an empty graph.
uint16_t minimed_graph_serialize(const MinimedGraph *graph, uint32_t window_secs, uint8_t *out);
