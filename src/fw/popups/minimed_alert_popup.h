/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

//! Post a pump alarm/alert, or the hypo model's predicted low, as a native notification (popup +
//! vibe + notification list). title is the alert name; body is optional context (NULL or "" to
//! omit), e.g. the latest BG. Body text past 159 characters is cut. Safe from any task, including the BT host task; the item is built on
//! KernelMain. Dismissal is watch-local only -- the pump keeps alarming until cleared there.
//! bypass_dnd: ring through Quiet Time/Do Not Disturb instead of being hidden/silenced with
//! everything else -- for an alert that must reach the wearer regardless (see issue #30).
void minimed_alert_popup_push(const char *title, const char *body, bool bypass_dnd);
