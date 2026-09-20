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
#include "minimed_glucose_announce.h"
#include "minimed_graph.h"
#include "pebble_glucose_protocol.h"
#include "process_management/app_manager.h"
#include "pbl/services/comm_session/protocol.h"
#include "pbl/services/comm_session/session_transport.h"
#include "popups/minimed_sake_ui.h"
#include <pbl/logging/logging.h>
#include "util/dict.h"
#include "util/net.h"
#include "pbl/util/size.h"
#include "pbl/util/uuid.h"

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Who we send to, and what they asked for -- both learned from the watchface's capability
// announcement rather than hardcoded, so any watchface implementing the Pebble Glucose Protocol
// works. Zeroed = nobody has announced yet, and we send nothing.
static Uuid s_target_uuid;
static bool s_have_target;
static uint32_t s_caps;
static uint8_t s_graph_hours;

// One-entry negative cache: a watchface we claimed (see prv_get_uuid) that turned out not to
// speak our protocol. One entry is enough -- there is one foreground app at a time -- and it
// costs that watchface exactly one swallowed-then-NACKed message before we get out of its way.
static Uuid s_not_glucose_uuid;
static bool s_have_not_glucose;

// Returned by prv_get_uuid when we do NOT want to claim the foreground app. It must be a *valid*
// UUID that cannot match: prv_get_app_session treats a session with an invalid UUID as a
// catch-all fallback (session.c prv_find_session_by_app_uuid_comparator), which would capture
// traffic meant for the phone. All-zeros is never compared against, because that function bails
// out before the walk when the foreground app's own UUID is system/invalid.
static const Uuid s_no_claim_uuid = UUID_SYSTEM;

#define BG_STR_MAX 8      // watchface buffer is 16; bridge sends "N.N"/"NN.N"/"---"
#define IOB_STR_MAX 8     // "N.N"/"NN.N" IU
#define STATUS_STR_MAX 20

// Enough for any capability announcement: 3 tuples of at most 7 + 4 bytes plus the count byte.
// Sized with slack so a future key or two still parses rather than being read as a foreign app.
#define ANNOUNCE_DICT_MAX 64

// Largest dictionary we serialize. The graph blob dominates; the rest is the BG/IOB/status
// strings, the timestamp, and a 7-byte Tuple header each (worst case 1 + 11 + 15 + 15 + 27 + 7 +
// blob). Sized with room to spare -- an undersized buffer is a silent "wf dict fail", not a
// crash, but it would also mean no data reaches the watchface at all.
#define WF_DICT_MAX (MINIMED_GRAPH_BLOB_MAX + 192)

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
static char s_iob_total_str[IOB_STR_MAX];
static char s_status_str[STATUS_STR_MAX];  // "" = normal (watchface hides the band)
static uint32_t s_status_start;
static uint32_t s_status_end;
static bool s_pump_connected;  // false (offline) is the correct default until the pump connects
static bool s_pred_valid;
static uint16_t s_pred_mgdl;
static bool s_meal_valid;
static uint16_t s_meal_grams;
static uint32_t s_meal_timestamp;
static bool s_trend_valid;     // false = no trend field in the last reading; omit the key
static uint8_t s_trend_arrow;  // one of the TREND_* constants; only meaningful if s_trend_valid

static MinimedGraph s_graph;

// True when a graph point was added since the last push. Serializing the graph on every push
// (BG/IOB/status) allocates a fresh ~100 B blob in KernelMain each time; under 0x101 bursts that
// accumulates faster than the watchface drains and was a contributor to the OOM crash (kernel heap
// draining to ~2.7 KB, 2026-09-09). Only re-serialize the graph when it changed, or on a full BG
// push (which the watchface treats as a fresh-data event and expects a graph with).
static bool s_graph_dirty;

// Push throttle: coalesce pushes that arrive within a short window into one. The pump's 0x101
// bursts can fire several reads per second, each calling send_bg/send_iob/send_status, each
// scheduling a push. If the watchface is busy (or not foreground) the injected frames sit in its
// app-inbox, which is a fixed 2048 B buffer in KernelMain -- under a burst it accumulates faster
// than the watchface drains and was a contributor to the OOM crash (kernel heap to ~2.7 KB,
// 2026-09-09). Pushes within 150 ms of the previous one are dropped; the next one carries the
// latest values, so nothing is lost, only coalesced.
#define PUSH_COALESCE_MS 150
static uint32_t s_last_push_ticks;

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
//
// `uuid` and the dictionary come straight from the frame, so the sender identifies itself: no
// need to re-ask app_manager which app is in the foreground.
static void prv_handle_watchface_push(uint8_t txn, const Uuid *uuid, const uint8_t *dict,
                                      uint16_t dict_len, bool truncated);

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
      if (msg.command == CMD_PUSH && payload_len >= offsetof(AppMessagePush, dictionary)) {
        // Copy out the UUID + as much of the dictionary as an announcement could possibly be.
        // A longer message is not one (ours is 3 tuples, ~28 B), but say so rather than letting
        // the parse fail on our own truncation -- see prv_handle_watchface_push.
        Uuid uuid;
        comm_session_send_queue_copy(s_session, sizeof(hdr) + offsetof(AppMessagePush, uuid),
                                     sizeof(uuid), (uint8_t *)&uuid);
        const uint16_t dict_len =
            (uint16_t)(payload_len - offsetof(AppMessagePush, dictionary));
        uint8_t dict[ANNOUNCE_DICT_MAX];
        const bool truncated = dict_len > sizeof(dict);
        const uint16_t copy_len = truncated ? sizeof(dict) : dict_len;
        comm_session_send_queue_copy(s_session, sizeof(hdr) + offsetof(AppMessagePush, dictionary),
                                     copy_len, dict);
        prv_handle_watchface_push(msg.transaction_id, &uuid, dict, copy_len, truncated);
      }
      // CMD_ACK/CMD_NACK (responses to our pushes): swallow silently.
    }
    comm_session_send_queue_consume(s_session, frame_len);
    remaining -= frame_len;
  }
}

static void prv_reset(Transport *transport) {}

// Forward: defined below with the session lifecycle. prv_close re-opens the loopback after a phone
// reconnect evicts it, which routes through this same mode callback.
static void prv_set_mode_cb(void *ctx);

// comm_session_open closes the *existing* system session when a new one connects (last-system-session
// wins), via transport->close. The loopback's get_type is QEMU, which prv_get_system_session treats
// as a last-resort system session, so a phone reconnect will call this to evict us. It must actually
// close the session (and clear our pointer), or PPoGATT hits "System session already exists and
// cannot be closed" and the phone loops connect/disconnect forever.
//
// After the eviction the loopback is gone, so in DUAL mode the watchface would stop receiving data.
// Re-open it once the phone's session is up: deferred to KernelMain so we don't race PPoGATT's own
// comm_session_open (which is mid-flight and holds bt_lock when this runs).
static void prv_close(Transport *transport) {
  bt_lock();
  if (s_session) {
    comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
    s_session = NULL;
  }
  bt_unlock();
  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    launcher_task_add_callback(prv_set_mode_cb, (void *)1);  // re-open the loopback
  }
}

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

// Which app this loopback speaks for. prv_get_app_session matches sessions against the foreground
// app's UUID, so whatever we return here decides whose outbound AppMessages reach us. Without a
// match the watchface's launch/reconnect ready ping falls back to the phone's Hybrid session --
// the ping never arrives, we never ACK + resend, and the watchface stays stale until the next
// poll happens to land while it is foreground.
//
// We claim the foreground app's own UUID, so ANY watchface's ping reaches us rather than only one
// hardcoded UUID. Two limits keep that from stealing traffic that isn't ours:
//   - watchfaces only. Settings and ordinary watchapps (ProcessTypeApp) are left alone entirely,
//     which is most of what could have been intercepted.
//   - the negative cache. A watchface that turns out not to speak our protocol is dropped after
//     one message (see prv_handle_watchface_push).
// Claiming has to come first: there is no way to receive the announcement that identifies a
// watchface without already being the session its messages route to.
static const Uuid *prv_claimed_uuid(void) {
  const PebbleProcessMd *md = app_manager_get_current_app_md();
  if (!md || md->process_type != ProcessTypeWatchface) {
    return &s_no_claim_uuid;
  }
  if (s_have_not_glucose && uuid_equal(&md->uuid, &s_not_glucose_uuid)) {
    return &s_no_claim_uuid;
  }
  return &md->uuid;
}

static const Uuid *prv_get_uuid(struct Transport *transport) { return prv_claimed_uuid(); }

static const TransportImplementation s_loopback_implementation = {
    .send_next = prv_send_next,
    .close = prv_close,
    .reset = prv_reset,
    .set_connection_responsiveness = prv_set_connection_responsiveness,
    .get_uuid = prv_get_uuid,
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

// KernelMain only. Push the stored data to the target watchface. Delivery silently no-ops when
// that watchface isn't the foreground app (inbox missing or UUID mismatch -> clean drop), so
// there is no foreground check here.
//
// Every push carries every announced field, not just the one that changed. The protocol allows
// sending only what is new, but this transport is a memcpy rather than a radio, so re-sending the
// whole frame costs nothing and keeps the watchface correct after a relaunch.
static void prv_push_bg_cb(void *unused) {
  // Coalesce bursts (see PUSH_COALESCE_MS). The injected frame would otherwise pile up in the
  // watchface's app-inbox when the watchface is busy or not foreground.
  const uint32_t now = (uint32_t)rtc_get_ticks();
  if (s_last_push_ticks != 0 && (now - s_last_push_ticks) < PUSH_COALESCE_MS) {
    PBL_LOG_INFO("wf push: dropped, within coalesce window");
    return;
  }
  s_last_push_ticks = now;

  if (!s_session) {
    PBL_LOG_INFO("wf push: dropped, no session");
    return;
  }

  // BG/IOB/status/graph have nothing to say until the first real reading arrives. Pump-connected
  // is different: false (offline) is itself a real, correct value before that, so it must not
  // wait behind this gate.
  const bool have_bg = (s_bg_str[0] != '\0');

  // Before anyone has announced, send everything to the foreground watchface. Waiting for an
  // announcement is the protocol-correct behaviour but loses the first one in practice: the
  // watchface announces at launch and on the app-connection event, and a boot that starts in
  // NORMAL has already done both by the time the mode toggle creates this session -- so the ping
  // routes to the phone and is never repeated (HW, v67, 2026-09-08). A watchface that does not
  // speak the protocol just ignores keys it doesn't know; this stops for good the moment any
  // watchface announces, after which we are exact.
  Uuid target;
  uint32_t caps;
  uint32_t graph_window_secs;
  bool send_graph;
  if (s_have_target) {
    target = s_target_uuid;
    caps = s_caps;
    graph_window_secs = (uint32_t)s_graph_hours * 60 * 60;
    send_graph = (s_graph_hours > 0);  // GRAPH_HOURS == 0 is how a watchface declines the graph
  } else {
    const Uuid *fg = prv_claimed_uuid();
    if (uuid_equal(fg, &s_no_claim_uuid)) {
      PBL_LOG_INFO("wf push: dropped, foreground is not a glucose watchface");
      return;  // not a watchface, or one we already know isn't ours
    }
    target = *fg;
    // Everything we can currently supply.
    caps = CAP_BG | CAP_IOB | CAP_STATUS | CAP_PUMP_CONNECTED | CAP_TREND_ARROW | CAP_MEAL |
           CAP_PREDICTION | CAP_IOB_TOTAL;
    graph_window_secs = MINIMED_GRAPH_MAX_HOURS * 60 * 60;
    send_graph = true;
  }

  AppMessagePush *push = (AppMessagePush *)FRAME_PAYLOAD;
  *push = (AppMessagePush){
      .header = {.command = CMD_PUSH, .transaction_id = s_txn++},
      .uuid = target,
  };

  uint8_t graph[MINIMED_GRAPH_BLOB_MAX];
  // Only re-serialize the graph when it changed since the last push, or on a BG push (the
  // watchface keys a fresh graph off a new BG). This avoids a KernelMain allocation on every
  // IOB/status-only push, which under bursts was draining the heap.
  bool graph_present = false;
  uint16_t graph_len = 0;
  if (have_bg && send_graph && (s_graph_dirty || (caps & CAP_BG))) {
    graph_len = minimed_graph_serialize(&s_graph, graph_window_secs, graph);
    s_graph_dirty = false;
    graph_present = true;
  }

  // Written key by key rather than from a Tuplet array: Tuplet's value union has const members,
  // so a conditionally-filled array can't be assigned into.
  DictionaryIterator iter;
  DictionaryResult res = dict_write_begin(&iter, (uint8_t *)&push->dictionary, WF_DICT_MAX);
  uint8_t n = 0;

  // Only the announced fields (or, pre-announcement, everything). A field a watchface did not ask
  // for is one it cannot render, and its key number may well mean something else there. BG/IOB/
  // status/graph additionally wait for have_bg -- there is nothing real to say there yet.
  if (have_bg && (caps & CAP_BG)) {
    res |= dict_write_uint32(&iter, KEY_BG_TIMESTAMP, s_bg_timestamp);
    res |= dict_write_cstring(&iter, KEY_BG_STRING, s_bg_str);
    n += 2;
  }
  if (have_bg && (caps & CAP_IOB)) {
    // Empty until the first IOB read; the watchface blanks the field.
    res |= dict_write_cstring(&iter, KEY_IOB_STRING, s_iob_str);
    n++;
  }
  if (have_bg && (caps & CAP_STATUS)) {
    res |= dict_write_cstring(&iter, KEY_STATUS_STRING, s_status_str);  // "" = normal, band hidden
    n++;
    if (s_status_start != 0) {
      res |= dict_write_uint32(&iter, KEY_STATUS_START, s_status_start);
      n++;
    }
    if (s_status_end != 0) {
      res |= dict_write_uint32(&iter, KEY_STATUS_END, s_status_end);
      n++;
    }
  }
  if (caps & CAP_PUMP_CONNECTED) {
    // Not gated on have_bg: offline (0) is a real value from boot, not a placeholder waiting on
    // the pump's first reading.
    res |= dict_write_uint8(&iter, KEY_PUMP_CONNECTED, s_pump_connected ? 1 : 0);
    n++;
  }
  if (have_bg && (caps & CAP_TREND_ARROW) && s_trend_valid) {
    // Omitted (not sent as TREND_UNKNOWN) when the pump didn't supply one -- see
    // minimed_sake_sender_send_trend_arrow.
    res |= dict_write_uint8(&iter, KEY_TREND_ARROW, s_trend_arrow);
    n++;
  }
  if (have_bg && s_iob_total_str[0] != '\0' && (caps & CAP_IOB_TOTAL)) {
    res |= dict_write_cstring(&iter, KEY_IOB_TOTAL_STRING, s_iob_total_str);
    n++;
  }
  if (have_bg && s_pred_valid && (caps & CAP_PREDICTION)) {
    res |= dict_write_uint16(&iter, KEY_PREDICTED_BG, s_pred_mgdl);
    n++;
  }
  if (s_meal_valid && (caps & CAP_MEAL)) {
    res |= dict_write_uint16(&iter, KEY_MEAL_CARBS, s_meal_grams);
    res |= dict_write_uint32(&iter, KEY_MEAL_TIMESTAMP, s_meal_timestamp);
    n += 2;
  }
  // Omit the graph key entirely until there is a point to plot -- a zero-length byte array would
  // tell the watchface that "count=0" is a real, parseable graph.
  if (graph_present && graph_len > 0) {
    res |= dict_write_data(&iter, KEY_GRAPH_DATA, graph, graph_len);
    n++;
  }
  if (n == 0) {
    PBL_LOG_INFO("wf push: dropped, nothing to send");
    return;  // a watchface that announced nothing we can currently supply
  }
  if (res != DICT_OK) {
    minimed_sake_log_evt("wf dict fail");
    return;
  }
  // Injection cannot report a drop (no inbox, wrong foreground app), so log whether the
  // target is in the foreground to tell a delivered push from a discarded one.
  const PebbleProcessMd *fg = app_manager_get_current_app_md();
  PBL_LOG_INFO("wf push: sent n=%u bg=%u fg_match=%d", (unsigned)n, (unsigned)have_bg,
               (int)(fg && uuid_equal(&fg->uuid, &target)));
  prv_inject(offsetof(AppMessagePush, dictionary) + dict_write_end(&iter));
}

// KernelMain only. ACK the watchface's ready ping (txn in ctx), then answer it with the data it
// asked for.
static void prv_ack_and_resend_cb(void *ctx) {
  AppMessageAck *ack = (AppMessageAck *)FRAME_PAYLOAD;
  *ack = (AppMessageAck){
      .header = {.command = CMD_ACK, .transaction_id = (uint8_t)(uintptr_t)ctx},
  };
  prv_inject(sizeof(*ack));
  minimed_sake_log_evt("wf ready ping");
  prv_push_bg_cb(NULL);
}

// KernelMain only. NACK a message that was not an announcement, so the app sees its send fail
// rather than the message vanishing. By now prv_get_uuid has stopped claiming this watchface, so
// its retry routes wherever it was meant to go.
static void prv_nack_cb(void *ctx) {
  AppMessageAck *nack = (AppMessageAck *)FRAME_PAYLOAD;
  *nack = (AppMessageAck){
      .header = {.command = CMD_NACK, .transaction_id = (uint8_t)(uintptr_t)ctx},
  };
  prv_inject(sizeof(*nack));
  minimed_sake_log_evt("not a glucose wf");
}

static void prv_handle_watchface_push(uint8_t txn, const Uuid *uuid, const uint8_t *dict,
                                      uint16_t dict_len, bool truncated) {
  MinimedGlucoseAnnounce announce;
  if (truncated || !minimed_glucose_parse_announce(dict, dict_len, &announce)) {
    // Not one of ours; fail the send. Blacklist it so prv_get_uuid stops claiming it -- but not
    // when we truncated the read, since that is us failing to look rather than the watchface
    // failing to announce, and a permanent blacklist is too harsh a price for our own buffer.
    if (!truncated) {
      s_not_glucose_uuid = *uuid;
      s_have_not_glucose = true;
    }
    launcher_task_add_callback(prv_nack_cb, (void *)(uintptr_t)txn);
    return;
  }
  // Confirmed. Everything we send from here on is addressed and shaped by this announcement.
  s_target_uuid = *uuid;
  s_have_target = true;
  s_caps = announce.caps;
  s_graph_hours = announce.graph_hours;
  // Only clear the cache for THIS watchface. Clearing it unconditionally would re-probe (and
  // re-NACK) a known non-match every time you switched back from the glucose watchface, since
  // returning to a watchface relaunches it and re-announces.
  if (s_have_not_glucose && uuid_equal(uuid, &s_not_glucose_uuid)) {
    s_have_not_glucose = false;
  }
  launcher_task_add_callback(prv_ack_and_resend_cb, (void *)(uintptr_t)txn);
}

// -- Session lifecycle --------------------------------------------------------------------------

// KernelMain only. ctx != NULL -> open (DUAL mode), NULL -> close (NORMAL mode).
// NOTE: we deliberately do NOT emit PEBBLE_BT_CONNECTION_EVENT here (tried in v19). The watchface's
// system "not connected" banner is a separate problem, tackled after pairing works; faking a
// connection at DUAL entry is also a discovery confound we want out of the way.
static void prv_set_mode_cb(void *ctx) {
  const bool open = (ctx != NULL);
  bt_lock();
  if (open && !s_session) {
    // TransportDestinationApp (not Hybrid): a Hybrid loopback is a "system" session and would
    // evict the real phone session on open (comm_session_open: last system session wins). In DUAL
    // mode both sessions must coexist, so use an App destination, which skips the eviction.
    s_session = comm_session_open((Transport *)&s_transport, &s_loopback_implementation,
                                  TransportDestinationApp);
    if (s_session) {
      comm_session_set_capabilities(s_session, CommSessionAppMessage8kSupport);
    }
    minimed_sake_log_evt(s_session ? "wf sender up" : "wf sender FAIL");
  } else if (!open && s_session) {
    comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
    s_session = NULL;
  }
  bt_unlock();
}

// -- Public API ---------------------------------------------------------------------------------

void minimed_sake_sender_send_bg(const char *bg_str, uint32_t timestamp) {
  // Written on the BT host task, consumed on KernelMain. A torn read would garble one displayed
  // value for one 60s poll cycle -- tolerable, matching the MiniMed pump link's lock-free logging approach.
  strncpy(s_bg_str, bg_str, sizeof(s_bg_str) - 1);
  s_bg_str[sizeof(s_bg_str) - 1] = '\0';
  s_bg_timestamp = timestamp;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_add_graph_point(uint32_t timestamp, int32_t mgdl) {
  // Runs on the BT host task; read on KernelMain during the push. Same lock-free discipline as the
  // BG string -- the worst case is one frame drawn from a half-updated array.
  minimed_graph_add(&s_graph, timestamp, mgdl);
  s_graph_dirty = true;
}

// Staging for a backfill: filled on the BT host task, consumed on KernelMain so the sorted inserts
// never run concurrently with a serialize. One backfill happens per connection, so a second call
// racing the first is not a case worth a lock.
static uint32_t s_backfill_ts[MINIMED_BACKFILL_MAX_POINTS];
static int32_t s_backfill_mgdl[MINIMED_BACKFILL_MAX_POINTS];
static uint8_t s_backfill_count;

static void prv_backfill_cb(void *unused) {
  for (uint8_t i = 0; i < s_backfill_count; i++) {
    minimed_graph_insert_past(&s_graph, s_backfill_ts[i], s_backfill_mgdl[i]);
  }
  s_backfill_count = 0;
  s_graph_dirty = true;
  prv_push_bg_cb(NULL);
}

void minimed_sake_sender_backfill_graph(const uint32_t *timestamps, const int32_t *mgdl,
                                        uint8_t count) {
  if (count > MINIMED_BACKFILL_MAX_POINTS) {
    count = MINIMED_BACKFILL_MAX_POINTS;
  }
  memcpy(s_backfill_ts, timestamps, count * sizeof(timestamps[0]));
  memcpy(s_backfill_mgdl, mgdl, count * sizeof(mgdl[0]));
  s_backfill_count = count;
  launcher_task_add_callback(prv_backfill_cb, NULL);
}

void minimed_sake_sender_send_iob(const char *iob_str) {
  // Same lock-free discipline as send_bg. Deliberately does NOT touch s_bg_timestamp: an IOB
  // update must not make a stale BG look fresh (the watchface keys staleness off the BG timestamp).
  strncpy(s_iob_str, iob_str, sizeof(s_iob_str) - 1);
  s_iob_str[sizeof(s_iob_str) - 1] = '\0';
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_status(const char *status_str, uint32_t start, uint32_t end) {
  // Same lock-free discipline as send_iob; likewise leaves s_bg_timestamp alone.
  strncpy(s_status_str, status_str, sizeof(s_status_str) - 1);
  s_status_str[sizeof(s_status_str) - 1] = '\0';
  s_status_start = start;
  s_status_end = end;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_set_mode(bool open) {
  launcher_task_add_callback(prv_set_mode_cb, open ? (void *)1 : NULL);
}

void minimed_sake_sender_send_pump_connected(bool connected) {
  // Same lock-free discipline as send_bg/send_iob/send_status: written here (BT host task or
  // KernelMain, depending on caller), read on KernelMain during the push.
  s_pump_connected = connected;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_total_iob(const char *iob_str) {
  // Same lock-free discipline as send_iob.
  strncpy(s_iob_total_str, iob_str, sizeof(s_iob_total_str) - 1);
  s_iob_total_str[sizeof(s_iob_total_str) - 1] = '\0';
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_prediction(bool valid, int32_t mgdl) {
  // Same lock-free discipline as the other setters.
  s_pred_valid = valid && mgdl > 0;
  s_pred_mgdl = (uint16_t)(mgdl > 0 ? (mgdl > 1000 ? 1000 : mgdl) : 0);
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_meal(uint16_t grams, uint32_t timestamp) {
  // Same lock-free discipline as the other setters.
  s_meal_grams = grams;
  s_meal_timestamp = timestamp;
  s_meal_valid = true;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}

void minimed_sake_sender_send_trend_arrow(bool valid, uint8_t trend) {
  // Same lock-free discipline as send_bg/send_iob/send_status.
  s_trend_valid = valid;
  s_trend_arrow = trend;
  launcher_task_add_callback(prv_push_bg_cb, NULL);
}
