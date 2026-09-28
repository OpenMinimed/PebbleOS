/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_task.h"

#include <pbl/drivers/task_watchdog.h>
#include <pbl/logging/logging.h>

#include "kernel/pebble_tasks.h"
#include "minimed_session.h"
#include "pbl/mcu/fpu.h"
#include "pbl/services/new_timer/new_timer.h"
#include "system/passert.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Deep enough for a 0x101 burst (a fingerstick fires ~5 indications in 15 s) plus the history
// fragments of a read, with the session a little behind. A drop is logged, not fatal: the op timeout
// and the 6-minute fallback poll recover from a lost event.
#define QUEUE_DEPTH 16

// PBL_LOG is stack-hungry, and the models run here. The host task these came from had 5000.
#define STACK_BYTES 4096

// Below apps and KernelMain: nothing here is latency-critical at the millisecond scale (the pump
// works on seconds), and the watchface UI must never wait on a model run.
#define TASK_PRIORITY (tskIDLE_PRIORITY + 1)

// The loop wakes at least this often to feed the task watchdog, so a session callback stuck for
// more than the watchdog's 6.5 s resets the watch with a coredump instead of silently stalling.
#define WATCHDOG_FEED_MS 2000

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
  task_watchdog_mask_set(PebbleTask_Minimed);
  for (;;) {
    static MinimedEvent s_event;  // one task, one event at a time: keep it off the stack
    if (xQueueReceive(s_queue, &s_event, pdMS_TO_TICKS(WATCHDOG_FEED_MS)) == pdTRUE &&
        prv_timer_event_is_current(&s_event)) {
      minimed_session_handle_event(&s_event);
      mcu_fpu_cleanup();  // the models use the FPU; don't carry its context into the next wait
    }
    task_watchdog_bit_set(PebbleTask_Minimed);
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
  s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(MinimedEvent));
  PBL_ASSERTN(s_queue);
  for (unsigned i = 0; i < MINIMED_TIMER_COUNT; i++) {
    s_timers[i] = new_timer_create();
  }
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
  if (xQueueSendToBack(s_queue, event, 0) != pdTRUE) {
    s_dropped++;
    return false;
  }
  return true;
}

void minimed_task_timer_start(uint8_t timer, uint32_t ms) {
  PBL_ASSERTN(timer < MINIMED_TIMER_COUNT);
  const uint8_t gen = ++s_timer_gen[timer];
  new_timer_start(s_timers[timer], ms, prv_timer_cb, (void *)(uintptr_t)(timer | (gen << 8)), 0);
}

void minimed_task_timer_stop(uint8_t timer) {
  PBL_ASSERTN(timer < MINIMED_TIMER_COUNT);
  s_timer_gen[timer]++;
  new_timer_stop(s_timers[timer]);
}
