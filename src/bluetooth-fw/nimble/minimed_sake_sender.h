/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Local AppMessage sender: feeds glucose data from the on-watch pump driver to an (unmodified)
//! watchface by injecting synthetic inbound AppMessages through a phone-less loopback
//! CommSession -- the same trick the QEMU transport uses. The watchface receives a byte-for-byte
//! normal AppMessage and cannot tell there is no phone.
//!
//! The target is discovered, not hardcoded: any watchface speaking the Pebble Glucose Protocol
//! (pebble_glucose_protocol.h) identifies itself with a capability announcement, and we then send
//! it exactly the fields it announced. Nothing is sent before that announcement arrives.

//! Latest BG for the watchface, e.g. "5.6" (pre-formatted display string, mmol/L). `timestamp` is
//! when the *sensor* produced this reading, not when we polled it -- re-polling an unchanged
//! reading must not make it look fresh, or the watchface's staleness display is meaningless.
//! Stores it and pushes it to the watchface if it is running; also re-pushed whenever the
//! watchface announces itself (launch/reconnect "ready" ping).
//! Safe to call from the BT host task; the actual send runs on KernelMain.
void minimed_sake_sender_send_bg(const char *bg_str, uint32_t timestamp);

//! Append one sensor reading to the graph history the watchface plots. Call once per NEW reading
//! (before send_bg, which is what actually pushes). Points older than the graph window are
//! dropped. History lives in RAM only: it survives a pump dropout and a mode toggle, but not a
//! reboot -- the watchface persists its own copy across relaunch.
void minimed_sake_sender_add_graph_point(uint32_t timestamp, int32_t mgdl);

//! Fill in readings from the past (e.g. the pump's event log after a connect). Unlike
//! add_graph_point these may lie anywhere in the window, in any order; points that duplicate one
//! already plotted are dropped. At most MINIMED_BACKFILL_MAX_POINTS are taken per call. Pushes the
//! updated graph to the watchface. Safe to call from the BT host task; the insert runs on
//! KernelMain.
#define MINIMED_BACKFILL_MAX_POINTS 56
void minimed_sake_sender_backfill_graph(const uint32_t *timestamps, const int32_t *mgdl,
                                        uint8_t count);

//! The predicted glucose 30 minutes after the current reading, mg/dL (KEY_PREDICTED_BG). `valid`
//! false clears it: the key is then omitted, and the watchface falls back to its own extrapolation.
//! Safe to call from the BT host task; the actual send runs on KernelMain.
void minimed_sake_sender_send_prediction(bool valid, int32_t mgdl);

//! A meal's carbohydrate amount, from the pump's history. `timestamp` is when the record was
//! logged. Kept in a small ring (MINIMED_MEAL_LIST_MAX, sorted oldest-first, oldest evicted once
//! full) and pushed as KEY_MEAL_LIST for a watchface that announced CAP_MEAL_LIST, and as the
//! newest entry's KEY_MEAL_CARBS/KEY_MEAL_TIMESTAMP for one that only announced CAP_MEAL. Call
//! once per meal (live or backfilled; a duplicate timestamp updates that entry rather than adding
//! one). Safe to call from the BT host task; the actual send runs on KernelMain.
void minimed_sake_sender_send_meal(uint16_t grams, uint32_t timestamp);

//! The hypo (treat-or-wait) model's recommendation for the current falling low, 0-100 (see
//! sugar_predictor/INTEGRATION.md's treat_pct). `valid` false clears it -- the key is then omitted,
//! for when the reading is no longer in the regime the model was fit for. Sent only to a
//! watchface that announced CAP_HYPO. Safe to call from the BT host task; the actual send runs on
//! KernelMain.
void minimed_sake_sender_send_hypo(bool valid, uint8_t treat_pct);

//! Latest insulin-on-board for the watchface, e.g. "2.5" (pre-formatted display string, IU). The
//! watchface adds the unit. Stored and pushed like the BG value, but does NOT advance the BG
//! timestamp (IOB and BG arrive from separate pump reads). Safe to call from the BT host task.
void minimed_sake_sender_send_iob(const char *iob_str);

//! Total IOB (pump IOB plus basal insulin still active) as "N.N"; an empty string withdraws it.
//! Sent only to a watchface that announced CAP_IOB_TOTAL. Same locking rules as send_iob.
void minimed_sake_sender_send_total_iob(const char *iob_str);

//! Update the pump-status line (status string and start/end times).
//! Pushes the full cached frame like send_iob; does not touch the BG timestamp.
void minimed_sake_sender_send_status(const char *status_str, uint32_t start, uint32_t end);

//! Open (open=true) / close (open=false) the loopback session. The session must NOT exist in
//! NORMAL mode: a real phone connection would then compete with it. Called from the mode toggle;
//! marshals to KernelMain internally.
void minimed_sake_sender_set_mode(bool open);

//! Report the pump link state for the watchface's connection indicator (KEY_PUMP_CONNECTED).
//! `connected` mirrors minimed_sake_pump_connected() and should be pushed on every transition, not
//! just polled -- offline is the default until the first "connected" call. Safe to call from the
//! BT host task; the actual send runs on KernelMain.
void minimed_sake_sender_send_pump_connected(bool connected);

//! Latest trend arrow (one of the TREND_* constants in pebble_glucose_protocol.h), decoded from
//! the pump's CGM Trend Information field -- never derived on the watch. `valid` is false when
//! the current reading carried no trend field (sensor warmup, older transmitter firmware, etc.);
//! the key is then omitted from the push entirely rather than sent as TREND_UNKNOWN, so the
//! watchface can tell "no trend" from "flat" and hide the glyph. Safe to call from the BT host
//! task; the actual send runs on KernelMain.
void minimed_sake_sender_send_trend_arrow(bool valid, uint8_t trend);
