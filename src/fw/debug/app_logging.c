/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/util/attributes.h"
#include <pbl/logging/logging.h>
#include "applib/app_logging.h"

#include <stdint.h>
#include <string.h>

#include "logging/logging_private.h"
#include "kernel/memory_layout.h"
#include "kernel/util/stack_info.h"
#include "pbl/mcu/interrupts.h"
#include "pbl/services/comm_session/session.h"
#include "process_management/app_manager.h"
#include "syscall/syscall_internal.h"

#include "FreeRTOS.h"
#include "task.h"

static const uint16_t APP_LOGGING_ENDPOINT = 2006;

static AppLoggingMode s_app_logging_mode = AppLoggingDisabled;

bool app_log_is_bt_enabled(void) {
  return s_app_logging_mode == AppLoggingEnabled;
}

static const uint32_t MIN_STACK_FOR_SEND_DATA = 400;

// Stamp the source so a flash-log dump (which otherwise mixes app lines in with the firmware's
// own PBL_LOG lines) makes it obvious at a glance which is which, instead of relying on someone
// recognising an app's C filename by name. Overwrites the filename field in place -- it is a
// fixed 16-byte buffer either way, so this just spends a few of those bytes on the tag instead of
// the tail of the path.
static void prv_tag_filename(LogBinaryMessage *log_msg, const char *tag) {
  char tagged[sizeof(log_msg->filename)];
  const size_t tag_len = strlen(tag) < sizeof(tagged) ? strlen(tag) : sizeof(tagged) - 1;
  memcpy(tagged, tag, tag_len);
  strncpy(tagged + tag_len, log_msg->filename, sizeof(tagged) - tag_len);
  tagged[sizeof(tagged) - 1] = '\0';
  memcpy(log_msg->filename, tagged, sizeof(tagged));
}

DEFINE_SYSCALL(void, sys_app_log, size_t length, void *log_buffer) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(log_buffer, length);
  }

  AppLogBinaryMessage *message = log_buffer;

  const PebbleProcessMd *md = app_manager_get_current_app_md();
  prv_tag_filename(&message->log_msg, (md && md->process_type == ProcessTypeWatchface)
                                          ? "WF:" : "APP:");

  // First log to serial, we always do this.
  kernel_pbl_log_serial(&message->log_msg, false);

  // Also persist to the flash log ring, same as firmware PBL_LOG lines. Previously app logs only
  // reached serial (needs a wired debug console) or a live BT listener (`pebble logs`), so a
  // flash-log dump pulled after the fact never showed what the watchapp itself saw or did, only
  // what the firmware sent it -- exactly backwards when the bug report is "the watchface didn't
  // update". Same context guard as kernel_pbl_log uses before its own flash write.
  if (!portIN_CRITICAL() && !mcu_state_is_isr() &&
      xTaskGetSchedulerState() != taskSCHEDULER_SUSPENDED) {
    kernel_pbl_log_flash(&message->log_msg, false);
  }

  // Now check to see if app logging is enabled over bluetooth.
  if (s_app_logging_mode == AppLoggingDisabled) {
    return;
  }

  // Then log to the app logging endpoint (if we have enough stack space)
  uint32_t stack_space = stack_free_bytes();
  if (stack_space > MIN_STACK_FOR_SEND_DATA) {
    CommSession *session = comm_session_get_system_session();
    if (session) {
      comm_session_send_data(session, APP_LOGGING_ENDPOINT, (uint8_t*)log_buffer, length, COMM_SESSION_DEFAULT_TIMEOUT);
    }
  }
}

void app_log_protocol_msg_callback(CommSession *session, const uint8_t *data, const size_t length) {
  typedef struct PACKED AppLogCommand {
    uint8_t commandType;
  } AppLogCommand;

  enum AppLogCommandType {
    APP_LOG_COMMAND_DISABLE_LOGGING = 0,
    APP_LOG_COMMAND_ENABLE_LOGGING = 1,
  };

  AppLogCommand *command = (AppLogCommand *)data;
  switch(command->commandType) {
  case APP_LOG_COMMAND_ENABLE_LOGGING:
    s_app_logging_mode = AppLoggingEnabled;
    break;
  case APP_LOG_COMMAND_DISABLE_LOGGING:
    s_app_logging_mode = AppLoggingDisabled;
    break;
  default:
    PBL_LOG_WRN("Invalid app log command 0x%x", command->commandType);
  }
}

