/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_sender.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "applib/app_message/app_message_internal.h"
#include "comm/bt_lock.h"
#include "drivers/rtc.h"
#include "kernel/event_loop.h"
#include "minimed_graph.h"
#include "pbl/services/comm_session/protocol.h"
#include "pbl/services/comm_session/session_transport.h"
#include "popups/minimed_sake_spike_ui.h"
#include "system/logging.h"
#include "util/dict.h"
#include "util/net.h"
#include "pbl/util/size.h"
#include "pbl/util/uuid.h"

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// The MiniMed watchface (minimed-pebble-watchface package.json). Pushes carry the target app's
// UUID; the firmware delivery path drops them cleanly (NACK) when this app is not in the
// foreground, so no foreground check is needed here.
static const Uuid s_watchface_uuid = {
    0x56, 0x7a, 0x3f, 0x6e, 0x97, 0xd0, 0x4f, 0x3a,
    0xb6, 0x3f, 0x91, 0x6a, 0x82, 0x13, 0xd2, 0x84,
};

// Pebble Glucose Protocol v1 keys (minimed-pebble-watchface docs/PEBBLE_GLUCOSE_PROTOCOL.md).
// BG string + timestamp; IOB string (14); graph (17). Status (15) later.
#define KEY_BG_TIMESTAMP 10
#define KEY_BG_STRING 11
#define KEY_IOB_STRING 14
#define KEY_STATUS_STRING 15
#define KEY_GRAPH_DATA 17

#define BG_STR_MAX 8      // watchface buffer is 16; bridge sends "N.N"/"NN.N"/"---"
#define IOB_STR_MAX 8     // "N.N"/"NN.N" IU
#define STATUS_STR_MAX 20  // watchface s_status_string is 20; longest label "TEMP TARGET H:MM"

// Largest dictionary we serialize. The graph blob dominates; the rest is the BG/IOB/status
// strings, the timestamp, and a 7-byte Tuple header each (worst case 1 + 11 + 15 + 15 + 27 + 7 +
// blob). Sized with room to spare -- an undersized buffer is a silent "wf dict fail", not a
// crash, but it would also mean no data reaches the watchface at all.
#define WF_DICT_MAX (MINIMED_GRAPH_BLOB_MAX + 128)

// All state below is only touched on KernelMain (every entry point marshals there), except the
// string/timestamp pair which is written before the marshal -- see minimed_sake_sender_send_bg.
typedef struct {
  Transport *unused;
} LoopbackTransport;
static LoopbackTransport s_transport;
static CommSession *s_session;
static uint8_t s_txn;
static char s_bg_str[BG_STR_MAX];
static uint32_t s_bg_timestamp;
static char s_iob_str[IOB_STR_MAX];
static char s_status_str[STATUS_STR_MAX];  // "" = normal (watchface hides the band)

static MinimedGraph s_graph;

// The one outbound frame: [PebbleProtocolHeader][AppMessagePush ... dictionary]. Static rather
// than two nested stack buffers -- with the graph blob that pair came to ~500 B of KernelMain
// stack. Every writer runs on KernelMain, so there is no concurrent use to guard against.
static uint8_t s_frame[sizeof(PebbleProtocolHeader) + offsetof(AppMessagePush, dictionary) +
                       WF_DICT_MAX];
#define FRAME_PAYLOAD (s_frame + sizeof(PebbleProtocolHeader))

// -- Outbound (watchface -> us): drain the send queue, detect the "ready" ping -----------------

// bt_lock held (called by the send queue when the watchface's outbox has data). The watchface
// sends a ready ping (a CMD_PUSH with protocol version + capabilities) on launch and on
// (believed) reconnect; replying with an ACK makes its outbox succeed, and we follow up with the
// latest BG. Its ACKs of OUR pushes also land here and must be ignored, or we'd loop forever.
static void prv_handle_watchface_push(uint8_t txn);

static void prv_send_next(Transport *transport) {
  if (!s_session) {
    return;
  }
  size_t remaining = comm_session_send_queue_get_length(s_session);
  while (remaining >= sizeof(PebbleProtocolHeader)) {
    PebbleProtocolHeader hdr;
    comm_session_send_queue_copy(s_session, 0, sizeof(hdr), (uint8_t *)&hdr);
    const uint16_t payload_len = ntohs(hdr.length);
    const uint16_t endpoint = ntohs(hdr.endpoint_id);
    const size_t frame_len = sizeof(hdr) + payload_len;
    if (remaining < frame_len) {
      break;  // partial frame; wait for the rest
    }
    if (endpoint == APP_MESSAGE_ENDPOINT_ID && payload_len >= sizeof(AppMessageHeader)) {
      AppMessageHeader msg;
      comm_session_send_queue_copy(s_session, sizeof(hdr), sizeof(msg), (uint8_t *)&msg);
      if (msg.command == CMD_PUSH) {
        prv_handle_watchface_push(msg.transaction_id);
      }
      // CMD_ACK/CMD_NACK (responses to our pushes): swallow silently.
    }
    comm_session_send_queue_consume(s_session, frame_len);
    remaining -= frame_len;
  }
}

static void prv_reset(Transport *transport) {}

static void prv_set_connection_responsiveness(Transport *transport, BtConsumer consumer,
                                              ResponseTimeState state, uint16_t max_period_secs,
                                              ResponsivenessGrantedHandler granted_handler) {
  if (granted_handler) {
    launcher_task_add_callback((void (*)(void *))granted_handler, NULL);
  }
}

static CommSessionTransportType prv_get_type(struct Transport *transport) {
  // Nothing queries this for behavior we rely on; QEMU is the closest "not a real radio" type.
  return CommSessionTransportType_QEMU;
}

static const TransportImplementation s_loopback_implementation = {
    .send_next = prv_send_next,
    .reset = prv_reset,
    .set_connection_responsiveness = prv_set_connection_responsiveness,
    .get_type = prv_get_type,
};

// -- Inbound injection (us -> watchface) --------------------------------------------------------

// Defined in session.c
extern void comm_session_set_capabilities(CommSession *session,
                                          CommSessionCapability capability_flags);

// KernelMain only. The caller has already built the payload at FRAME_PAYLOAD; stamp the header in
// front of it and inject through the same inbound router the phone uses, so the watchface receives
// a completely normal AppMessage.
static void prv_inject(uint16_t payload_len) {
  if (payload_len > sizeof(s_frame) - sizeof(PebbleProtocolHeader)) {
    return;
  }
  PebbleProtocolHeader *hdr = (PebbleProtocolHeader *)s_frame;
  hdr->length = htons(payload_len);
  hdr->endpoint_id = htons(APP_MESSAGE_ENDPOINT_ID);

  bt_lock();
  if (s_session) {
    comm_session_receive_router_write(s_session, s_frame, sizeof(*hdr) + payload_len);
  }
  bt_unlock();
}

// KernelMain only. Push the stored BG (if any) to the watchface. Delivery silently no-ops when
// the watchface isn't the foreground app (inbox missing or UUID mismatch -> clean drop).
static void prv_push_bg_cb(void *unused) {
  if (!s_session || s_bg_str[0] == '\0') {
    return;
  }

  AppMessagePush *push = (AppMessagePush *)FRAME_PAYLOAD;
  *push = (AppMessagePush){
      .header = {.command = CMD_PUSH, .transaction_id = s_txn++},
      .uuid = s_watchface_uuid,
  };

  uint8_t graph[MINIMED_GRAPH_BLOB_MAX];
  const uint16_t graph_len = minimed_graph_serialize(&s_graph, graph);

  uint32_t dict_size = WF_DICT_MAX;
  // Pointer locals: an array would trip -Werror=address in TupletCString's NULL check.
  const char *bg = s_bg_str;
  const char *iob = s_iob_str;
  const char *status = s_status_str;
  const Tuplet tuplets[] = {
      TupletInteger(KEY_BG_TIMESTAMP, s_bg_timestamp),
      TupletCString(KEY_BG_STRING, bg),
      TupletCString(KEY_IOB_STRING, iob),  // empty until the first IOB read; watchface blanks it
      TupletCString(KEY_STATUS_STRING, status),  // "" = normal; watchface hides the band
      // Graph rides every push rather than only on change: this transport is a memcpy, not a
      // radio, so re-sending ~100 B costs nothing and keeps the watchface in sync after a relaunch.
      TupletBytes(KEY_GRAPH_DATA, graph, graph_len),
  };
  // Drop the graph tuplet entirely until there is a point to plot -- a zero-length byte array
  // would tell the watchface "count=0" is a real, parseable graph.
  const uint8_t n_tuplets = ARRAY_LENGTH(tuplets) - (graph_len == 0 ? 1 : 0);
  if (dict_serialize_tuplets_to_buffer(tuplets, n_tuplets, (uint8_t *)&push->dictionary,
                                       &dict_size) != DICT_OK) {
    minimed_sake_log("wf dict fail");
    return;
  }
  prv_inject(offsetof(AppMessagePush, dictionary) + dict_size);
}

// KernelMain only. ACK the watchface's ready ping (txn in ctx), then answer it with the BG.
static void prv_ack_and_resend_cb(void *ctx) {
  AppMessageAck *ack = (AppMessageAck *)FRAME_PAYLOAD;
  *ack = (AppMessageAck){
      .header = {.command = CMD_ACK, .transaction_id = (uint8_t)(uintptr_t)ctx},
  };
  prv_inject(sizeof(*ack));
  minimed_sake_log("wf ready ping");
  prv_push_bg_cb(NULL);
}

static void prv_handle_watchface_push(uint8_t txn) {
  launcher_task_add_callback(prv_ack_and_resend_cb, (void *)(uintptr_t)txn);
}

// -- Session lifecycle --------------------------------------------------------------------------

// KernelMain only. ctx != NULL -> open (SPIKE mode), NULL -> close (NORMAL mode).
// NOTE: we deliberately do NOT emit PEBBLE_BT_CONNECTION_EVENT here (tried in v19). The watchface's
// system "not connected" banner is a separate problem, tackled after pairing works; faking a
// connection at SPIKE-entry is also a discovery confound we want out of the way.
static void prv_set_mode_cb(void *ctx) {
  const bool open = (ctx != NULL);
  bt_lock();
  if (open && !s_session) {
    s_session = comm_session_open((Transport *)&s_transport, &s_loopback_implementation,
                                  TransportDestinationHybrid);
    if (s_session) {
      comm_session_set_capabilities(s_session, CommSessionAppMessage8kSupport);
    }
    minimed_sake_log(s_session ? "wf sender up" : "wf sender FAIL");
  } else if (!open && s_session) {
    comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
    s_session = NULL;
  }
  bt_unlock();
}

// -- Public API ---------------------------------------------------------------------------------

void minimed_sake_sender_send_bg(const char *bg_str, uint32_t timestamp) {
  // Written on the BT host task, consumed on KernelMain. A torn read would garble one displayed
  // value for one 60s poll cycle -- tolerable, matching the spike's lock-free logging approach.
  strncpy(s_bg_str, bg_str, sizeof(s_bg_str) - 1);
  s_bg_str[sizeof(s_bg_str) - 1] = '\0';
  s_bg_timestamp = timestamp;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_add_graph_point(uint32_t timestamp, int32_t mgdl) {
  // Runs on the BT host task; read on KernelMain during the push. Same lock-free discipline as the
  // BG string -- the worst case is one frame drawn from a half-updated array.
  minimed_graph_add(&s_graph, timestamp, mgdl);
}

void minimed_sake_sender_send_iob(const char *iob_str) {
  // Same lock-free discipline as send_bg. Deliberately does NOT touch s_bg_timestamp: an IOB
  // update must not make a stale BG look fresh (the watchface keys staleness off the BG timestamp).
  strncpy(s_iob_str, iob_str, sizeof(s_iob_str) - 1);
  s_iob_str[sizeof(s_iob_str) - 1] = '\0';
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_status(const char *status_str) {
  // Same lock-free discipline as send_iob; likewise leaves s_bg_timestamp alone.
  strncpy(s_status_str, status_str, sizeof(s_status_str) - 1);
  s_status_str[sizeof(s_status_str) - 1] = '\0';
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_set_mode(bool spike) {
  launcher_task_add_callback(prv_set_mode_cb, spike ? (void *)1 : NULL);
}
