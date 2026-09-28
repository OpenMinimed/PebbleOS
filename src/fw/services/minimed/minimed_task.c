/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_task.h"

#include <pbl/logging/logging.h>

#include "drivers/rtc.h"
#include "kernel/pebble_tasks.h"
#include "minimed_session.h"
#include "pbl/mcu/fpu.h"
#include "pbl/services/new_timer/new_timer.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Deep enough for a 0x101 burst (a fingerstick fires ~5 indications in 15 s) plus the history
// fragments of a read, with the session a little behind.
#define QUEUE_DEPTH 24

// Timer and link events must never be dropped: the poll and watchdog timers re-arm only from
// their own handler, so a lost expiry would stop polling for good. Each timer has at most one
// expiry outstanding and link changes are rare, so this many slots are held back for them. A
// transport result that finds only the reserve left is dropped instead; the op timeout and the
// fallback poll recover from that.
#define RESERVED_SLOTS (MINIMED_TIMER_COUNT + 2)

// PBL_LOG is stack-hungry, and the models run here. The host task these came from had 5000.
#define STACK_BYTES 4096

// Below apps and KernelMain: nothing here is latency-critical at the millisecond scale (the pump
// works on seconds), and the watchface UI must never wait on a model run.
#define TASK_PRIORITY (tskIDLE_PRIORITY + 1)

// Deliberately NOT under the task watchdog: a stalled pump session must cost glucose data, never
// a watch reset. An event that takes this long is logged instead.
#define SLOW_EVENT_MS 1000

static QueueHandle_t s_queue;
static TimerID s_timers[MINIMED_TIMER_COUNT];
static uint32_t s_dropped;

// A timer can fire and queue its event just before the session stops or restarts it. Each start
// and stop bumps the timer's generation, the event carries the generation it was armed with, and
// the loop drops one that no longer matches -- otherwise a stale op timeout would kill the next op.
static uint8_t s_timer_gen[MINIMED_TIMER_COUNT];

static bool prv_timer_event_is_current(const MinimedEvent *event) {
  return event->type != MinimedEventTimer || event->tag >= MINIMED_TIMER_COUNT ||
         event->len == s_timer_gen[event->tag];
}

static void prv_task_main(void *unused) {
  for (;;) {
    static MinimedEvent s_event;  // one task, one event at a time: keep it off the stack
    if (xQueueReceive(s_queue, &s_event, portMAX_DELAY) != pdTRUE ||
        !prv_timer_event_is_current(&s_event)) {
      continue;
    }
    const RtcTicks start = rtc_get_ticks();
    minimed_session_handle_event(&s_event);
    mcu_fpu_cleanup();  // the models use the FPU; don't carry its context into the next wait
    const uint32_t ms = (uint32_t)(((rtc_get_ticks() - start) * 1000) / RTC_TICKS_HZ);
    if (ms >= SLOW_EVENT_MS) {
      PBL_LOG_WRN("minimed: event type %u took %lu ms", (unsigned)s_event.type,
                  (unsigned long)ms);
    }
    if (s_dropped != 0) {
      PBL_LOG_WRN("minimed: event queue full, dropped %lu", (unsigned long)s_dropped);
      s_dropped = 0;
    }
  }
}

// Runs on the NewTimers task. `data` packs the timer id (low byte) and its generation.
static void prv_timer_cb(void *data) {
  const uintptr_t packed = (uintptr_t)data;
  const MinimedEvent event = {
      .type = MinimedEventTimer,
      .tag = (uint8_t)packed,
      .len = (uint8_t)(packed >> 8),  // the generation; timer events carry no data
  };
  minimed_task_post(&event);
}

void minimed_task_init(void) {
  if (s_queue) {
    return;
  }
  // Synchronously, before anything can post: the settings must be loaded before the first
  // watchface announcement can change them.
  minimed_session_init();
  // Fail soft: without a queue or its timers the pump link just stays off. Nothing here may take
  // the watch down with it.
  for (unsigned i = 0; i < MINIMED_TIMER_COUNT; i++) {
    s_timers[i] = new_timer_create();
    if (s_timers[i] == TIMER_INVALID_ID) {
      PBL_LOG_ERR("minimed: no timers, pump link disabled");
      return;
    }
  }
  QueueHandle_t queue = xQueueCreate(QUEUE_DEPTH, sizeof(MinimedEvent));
  if (!queue) {
    PBL_LOG_ERR("minimed: no event queue, pump link disabled");
    return;
  }
  s_queue = queue;
  TaskParameters_t params = {
      .pvTaskCode = prv_task_main,
      .pcName = "Minimed",
      .usStackDepth = STACK_BYTES / sizeof(StackType_t),
      .uxPriority = TASK_PRIORITY | portPRIVILEGE_BIT,
      .puxStackBuffer = NULL,
  };
  pebble_task_create(PebbleTask_Minimed, &params, NULL);
}

bool minimed_task_post(const MinimedEvent *event) {
  if (!s_queue) {
    return false;
  }
  const bool reserved = event->type == MinimedEventTimer || event->type == MinimedEventLinkUp ||
                        event->type == MinimedEventLinkDown;
  if ((!reserved && uxQueueSpacesAvailable(s_queue) <= RESERVED_SLOTS) ||
      xQueueSendToBack(s_queue, event, 0) != pdTRUE) {
    s_dropped++;
    return false;
  }
  return true;
}

void minimed_task_timer_start(uint8_t timer, uint32_t ms) {
  if (timer >= MINIMED_TIMER_COUNT || !s_queue) return;
  const uint8_t gen = ++s_timer_gen[timer];
  new_timer_start(s_timers[timer], ms, prv_timer_cb, (void *)(uintptr_t)(timer | (gen << 8)), 0);
}

void minimed_task_timer_stop(uint8_t timer) {
  if (timer >= MINIMED_TIMER_COUNT || !s_queue) return;
  s_timer_gen[timer]++;
  new_timer_stop(s_timers[timer]);
}
