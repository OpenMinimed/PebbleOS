/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_spike_ui.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE

#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"

#include <stdio.h>
#include <string.h>

#define SAKE_LOG_LINES 8
#define SAKE_LOG_WIDTH 32

// Log state is mutated only on KernelMain (via prv_append_cb); the app task only reads s_joined,
// where a torn read is at worst a few garbage chars in a debug line -- acceptable, so no lock.
static char s_lines[SAKE_LOG_LINES][SAKE_LOG_WIDTH];
static int s_line_count;
static char s_joined[SAKE_LOG_LINES * SAKE_LOG_WIDTH];

static MinimedSakeMode s_mode = MinimedSakeModeNormal;

static void prv_rebuild_joined(void) {
  size_t off = 0;
  s_joined[0] = '\0';
  for (int i = 0; i < s_line_count && off < sizeof(s_joined); i++) {
    int n = snprintf(s_joined + off, sizeof(s_joined) - off, "%s\n", s_lines[i]);
    if (n < 0) {
      break;
    }
    off += n;
  }
}

static void prv_append_cb(void *data) {
  char *msg = (char *)data;
  char *slot;
  if (s_line_count < SAKE_LOG_LINES) {
    slot = s_lines[s_line_count++];
  } else {
    for (int i = 1; i < SAKE_LOG_LINES; i++) {
      memcpy(s_lines[i - 1], s_lines[i], SAKE_LOG_WIDTH);
    }
    slot = s_lines[SAKE_LOG_LINES - 1];
  }
  strncpy(slot, msg, SAKE_LOG_WIDTH - 1);
  slot[SAKE_LOG_WIDTH - 1] = '\0';
  prv_rebuild_joined();
  kernel_free(msg);
}

static const char *prv_stage_text(MinimedSakeStage stage) {
  switch (stage) {
    case MinimedSakeStageAdvertising:  return "advertising";
    case MinimedSakeStageConnected:    return "connected";
    case MinimedSakeStageEncrypted:    return "paired/encrypted";
    case MinimedSakeStageSubscribed:   return "subscribed";
    case MinimedSakeStageWrote:        return "PUMP WROTE!";
    case MinimedSakeStageHandshakeComplete: return "HANDSHAKE OK!";
    case MinimedSakeStageDisconnected: return "disconnected";
    default:                           return "?";
  }
}

void minimed_sake_log(const char *msg) {
  size_t n = strlen(msg) + 1;
  char *copy = (char *)kernel_malloc(n);
  if (!copy) {
    return;
  }
  memcpy(copy, msg, n);
  launcher_task_add_callback(prv_append_cb, copy);
}

// Deliberately silent. These stages used to buzz the motor (short on connect/subscribe, double on
// the handshake milestones) back when reaching them at all was the news. Now the pump re-handshakes
// on every reconnect, so a night of dropouts is a night of buzzing -- and these are raw vibes_*
// calls that ignore Quiet Time. The on-watch log is the debugging channel.
void minimed_sake_spike_report(MinimedSakeStage stage) { minimed_sake_log(prv_stage_text(stage)); }

MinimedSakeMode minimed_sake_get_mode(void) { return s_mode; }

const char *minimed_sake_get_log(void) { return s_joined; }

void minimed_sake_toggle_mode(void) {
  s_mode = (s_mode == MinimedSakeModeNormal) ? MinimedSakeModeSpike : MinimedSakeModeNormal;
  const bool spike = (s_mode == MinimedSakeModeSpike);
  minimed_sake_log(spike ? "mode -> SPIKE" : "mode -> NORMAL");
  minimed_sake_apply_sm_config(spike);  // pump legacy-JW in SPIKE, stock strict LESC for the phone
  minimed_sake_sender_set_mode(spike);
  minimed_sake_force_readvertise();
}

#else

MinimedSakeMode minimed_sake_get_mode(void) { return MinimedSakeModeNormal; }
void minimed_sake_toggle_mode(void) {}
void minimed_sake_spike_report(MinimedSakeStage stage) { (void)stage; }
void minimed_sake_log(const char *msg) { (void)msg; }
const char *minimed_sake_get_log(void) { return ""; }

#endif
