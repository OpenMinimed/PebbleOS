/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_alert_popup.h"

#include <string.h>

#include "drivers/rtc.h"
#include "kernel/event_loop.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/timeline.h"
#include "pbl/services/timeline/timeline_resources.h"

// Pending alert texts, written on the caller's task and drained on KernelMain (same lock-free
// discipline as minimed_sake_sender). Sized for a worst-case indication burst; overflow drops
// the oldest -- the pump itself still alarms, the watch is a mirror.
#define SLOT_TEXT_MAX 32
#define SLOT_COUNT 4
static char s_slots[SLOT_COUNT][SLOT_TEXT_MAX];
static volatile uint8_t s_head, s_tail;  // tail written by producer, head by KernelMain

static void prv_push_cb(void *unused) {
  while (s_head != s_tail) {
    const char *text = s_slots[s_head % SLOT_COUNT];

    AttributeList attr_list = {};
    attribute_list_add_cstring(&attr_list, AttributeIdTitle, "MiniMed");
    attribute_list_add_cstring(&attr_list, AttributeIdBody, text);
    attribute_list_add_uint32(&attr_list, AttributeIdIconTiny,
                              TIMELINE_RESOURCE_NOTIFICATION_GENERIC);

    AttributeList dismiss_attr_list = {};
    attribute_list_add_cstring(&dismiss_attr_list, AttributeIdTitle, "Dismiss");

    TimelineItemActionGroup action_group = {
      .num_actions = 1,
      .actions = (TimelineItemAction[]) {
        {
          .id = 0,
          .type = TimelineItemActionTypeDismiss,
          .attr_list = dismiss_attr_list,
        },
      },
    };

    TimelineItem *item = timeline_item_create_with_attributes(rtc_get_time(), 0,
                                                              TimelineItemTypeNotification,
                                                              LayoutIdNotification, &attr_list,
                                                              &action_group);
    attribute_list_destroy_list(&attr_list);
    attribute_list_destroy_list(&dismiss_attr_list);

    if (item) {
      notifications_add_notification(item);
      timeline_item_destroy(item);
    }
    s_head++;
  }
}

void minimed_alert_popup_push(const char *text) {
  if ((uint8_t)(s_tail - s_head) >= SLOT_COUNT) {
    s_head++;  // full: drop the oldest queued alert
  }
  char *slot = s_slots[s_tail % SLOT_COUNT];
  strncpy(slot, text, SLOT_TEXT_MAX - 1);
  slot[SLOT_TEXT_MAX - 1] = '\0';
  s_tail++;
  launcher_task_add_callback(prv_push_cb, NULL);
}
