/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Local AppMessage sender: feeds glucose data from the on-watch pump driver to the (unmodified)
//! MiniMed watchface by injecting synthetic inbound AppMessages through a phone-less loopback
//! CommSession -- the same trick the QEMU transport uses. The watchface receives a byte-for-byte
//! normal AppMessage and cannot tell there is no phone.

//! Latest BG for the watchface, e.g. "5.6" (pre-formatted display string, mmol/L). Timestamped
//! with the current time internally. Stores it and pushes it to the watchface if it is running;
//! also re-pushed whenever the watchface announces itself (launch/reconnect "ready" ping).
//! Safe to call from the BT host task; the actual send runs on KernelMain.
void minimed_sake_sender_send_bg(const char *bg_str);

//! Open (spike=true) / close (spike=false) the loopback session. The session must NOT exist in
//! NORMAL mode: a real phone connection would then compete with it. Called from the mode toggle;
//! marshals to KernelMain internally.
void minimed_sake_sender_set_mode(bool spike);
