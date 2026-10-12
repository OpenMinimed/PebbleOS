/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "applib/fonts/fonts.h"
#include "pbl/services/notifications/notifications.h"
#include "kernel/events.h"

void notification_window_service_init(void);

void notification_window_init(bool is_modal);

void notification_window_show(void);

bool notification_window_is_modal(void);

void notification_window_handle_notification(PebbleSysNotificationEvent *e);

//! Exempt the next notification with this id from Quiet Time's "hide notifications" setting and
//! vibrate/backlight mask, for an alert that must reach the wearer regardless (a safety alert,
//! not a chat message). Call with the id notifications_add_notification is about to be given;
//! consumed (and cleared) the first time that id is handled, so it only ever affects that one
//! notification.
void notification_window_bypass_dnd(const Uuid *id);

void notification_window_handle_reminder(PebbleReminderEvent *e);

void notification_window_handle_dnd_event(PebbleDoNotDisturbEvent *e);

void notification_window_add_notification_by_id(Uuid *id);

void notification_window_focus_notification(Uuid *id, bool animated);

void notification_window_mark_focused_read(void);

void app_notification_window_add_new_notification_by_id(Uuid *id);

void app_notification_window_remove_notification_by_id(Uuid *id);

void app_notification_window_handle_notification_acted_upon_by_id(Uuid *id);
