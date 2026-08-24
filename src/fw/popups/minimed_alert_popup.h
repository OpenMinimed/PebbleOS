/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Post a pump alarm/alert as a native notification (popup + vibe + notification list).
//! title is the alert name (pump wording); body is optional context (NULL or "" to omit),
//! e.g. the latest BG. Safe from any task, including the BT host task; the item is built on
//! KernelMain. Dismissal is watch-local only -- the pump keeps alarming until cleared there.
void minimed_alert_popup_push(const char *title, const char *body);
