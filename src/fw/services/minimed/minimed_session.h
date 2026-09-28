/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "minimed_task.h"

//! The pump session (minimed_session.c). minimed_task.c is the sole caller.

//! One-time setup, from minimed_task_init (the caller's task, before the MiniMed task exists).
void minimed_session_init(void);

//! Handle one event from the transport or a timer. MiniMed task only.
void minimed_session_handle_event(const MinimedEvent *event);
