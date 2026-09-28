/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "minimed_transport.h"

//! The MiniMed session's own task (PebbleTask_Minimed). Everything the session does -- the
//! exchange serialiser, parsing, the predictor and hypo models, the watchface sender's setters --
//! runs here, off the Bluetooth host task. The transport and the timers only post events.
//!
//! This is also the shape a watchapp port takes: a background worker that owns the pump link and
//! receives BLE callbacks and timer ticks as events.

//! Largest payload an event carries. Pump notifications are at most 67 bytes on the wire (the
//! history fragment the session decrypts into 64 bytes); longer ones are cut and flagged.
#define MINIMED_EVENT_DATA_MAX 80

typedef enum {
  MinimedEventLinkUp,      //!< SAKE handshake done; the pump link is ready for GATT client use
  MinimedEventLinkDown,    //!< the pump disconnected
  MinimedEventDiscovered,  //!< minimed_transport_discover finished (data unused)
  MinimedEventNotify,      //!< notification or indication on `chr`
  MinimedEventGattDone,    //!< a read, write or subscribe with `tag` finished; reads carry data
  MinimedEventTimer,       //!< timer `tag` fired
} MinimedEventType;

typedef struct {
  uint8_t type;       //!< MinimedEventType
  uint8_t chr;        //!< MinimedChr, for Notify and GattDone
  uint8_t tag;        //!< GattDone: the caller's tag; Timer: the timer id
  bool truncated;     //!< data was longer than MINIMED_EVENT_DATA_MAX
  MinimedGattStatus status;  //!< GattDone
  uint16_t len;
  uint8_t data[MINIMED_EVENT_DATA_MAX];
} MinimedEvent;

//! Timer ids are the session's own (minimed_session.c); the task only needs the count.
#define MINIMED_TIMER_COUNT 12

//! Create the task, its queue and timers. Call once, before any post.
void minimed_task_init(void);

//! Queue an event for the session. Safe from any task, never blocks; returns false (and counts
//! the drop) if the queue is full.
bool minimed_task_post(const MinimedEvent *event);

//! Post a MinimedEventTimer with `tag` = `timer` after `ms`. Restarts a running timer. Only
//! called on the MiniMed task.
void minimed_task_timer_start(uint8_t timer, uint32_t ms);

//! Stop a timer. Only called on the MiniMed task.
void minimed_task_timer_stop(uint8_t timer);
