/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

// Stand-ins for fw/services/voice and fw/services/voice_endpoint when CONFIG_MINIMED_STUB_VOICE
// leaves them out. Dictation fails the way it does on a board without a microphone: the session
// never starts, and the dictation window reports DictationSessionStatusFailureInternalError.

#include <stddef.h>
#include <stdint.h>

#include <pbl/services/comm_session/session.h>
#include <pbl/services/voice/voice.h>

#include <syscall/syscall_internal.h>

void voice_init(void) {
}

void voice_kill_app_session(PebbleTask task) {
}

DEFINE_SYSCALL(VoiceSessionId, sys_voice_start_dictation, VoiceEndpointSessionType session_type) {
  return VOICE_SESSION_ID_INVALID;
}

DEFINE_SYSCALL(void, sys_voice_stop_dictation, VoiceSessionId session_id) {
}

DEFINE_SYSCALL(void, sys_voice_cancel_dictation, VoiceSessionId session_id) {
}

void voice_endpoint_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t size) {
}
