/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Local AppMessage sender: feeds glucose data from the on-watch pump driver to the (unmodified)
//! MiniMed watchface by injecting synthetic inbound AppMessages through a phone-less loopback
//! CommSession -- the same trick the QEMU transport uses. The watchface receives a byte-for-byte
//! normal AppMessage and cannot tell there is no phone.

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

//! Latest insulin-on-board for the watchface, e.g. "2.5" (pre-formatted display string, IU). The
//! watchface adds the unit. Stored and pushed like the BG value, but does NOT advance the BG
//! timestamp (IOB and BG arrive from separate pump reads). Safe to call from the BT host task.
void minimed_sake_sender_send_iob(const char *iob_str);

//! Update the pump-status line (watchface key 15). "" = normal, the watchface hides the band.
//! Pushes the full cached frame like send_iob; does not touch the BG timestamp.
void minimed_sake_sender_send_status(const char *status_str);

//! Open (spike=true) / close (spike=false) the loopback session. The session must NOT exist in
//! NORMAL mode: a real phone connection would then compete with it. Called from the mode toggle;
//! marshals to KernelMain internally.
void minimed_sake_sender_set_mode(bool spike);
