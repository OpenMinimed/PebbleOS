/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_glucose_announce.h"

#include <string.h>

#include "pebble_glucose_protocol.h"

// Serialized dictionary layout (dict.h): [u8 count] then `count` tuples of
// [u32 key][u8 type][u16 length][length bytes of value], all little-endian. Re-read here rather
// than including dict.h so this file stays host-buildable.
#define TUPLE_HEADER_LEN 7
#define TYPE_UINT 2

#define CAP_ALL \
  (CAP_BG | CAP_TREND_ARROW | CAP_DELTA | CAP_IOB | CAP_STATUS | CAP_SENDER_BATTERY)

static uint16_t prv_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t prv_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read a tuple's value as an unsigned integer. Widths 1/2/4 are all accepted: the protocol says
// uint32 for CAPABILITIES, but a sender writing dict_write_uint8 for a small value is producing
// the same number, and rejecting it would be pedantry. The value constraints below do the real
// discriminating work.
static bool prv_uint_value(uint8_t type, uint16_t length, const uint8_t *val, uint32_t *out) {
  if (type != TYPE_UINT) {
    return false;
  }
  switch (length) {
    case 1: *out = val[0]; return true;
    case 2: *out = prv_rd16(val); return true;
    case 4: *out = prv_rd32(val); return true;
    default: return false;
  }
}

bool minimed_glucose_parse_announce(const uint8_t *dict, uint16_t len,
                                    MinimedGlucoseAnnounce *out) {
  memset(out, 0, sizeof(*out));
  if (len < 1) {
    return false;
  }

  bool have_version = false, have_caps = false;
  uint32_t caps = 0, graph_hours = 0, version = 0;

  const uint8_t count = dict[0];
  uint16_t off = 1;
  for (uint8_t i = 0; i < count; i++) {
    if (off + TUPLE_HEADER_LEN > len) {
      return false;  // truncated header
    }
    const uint32_t key = prv_rd32(dict + off);
    const uint8_t type = dict[off + 4];
    const uint16_t length = prv_rd16(dict + off + 5);
    const uint8_t *val = dict + off + TUPLE_HEADER_LEN;
    if (off + TUPLE_HEADER_LEN + length > len) {
      return false;  // truncated value
    }
    off += TUPLE_HEADER_LEN + length;

    uint32_t v;
    switch (key) {
      case KEY_PROTOCOL_VERSION:
        if (!prv_uint_value(type, length, val, &v)) return false;
        version = v;
        have_version = true;
        break;
      case KEY_CAPABILITIES:
        if (!prv_uint_value(type, length, val, &v)) return false;
        caps = v;
        have_caps = true;
        break;
      case KEY_GRAPH_HOURS:
        if (!prv_uint_value(type, length, val, &v)) return false;
        graph_hours = v;
        break;
      default:
        break;  // unknown keys are ignored, so the protocol can grow
    }
  }

  // The identification test. A foreign watchface would have to put exactly PROTOCOL_VERSION in
  // key 0 and a capability-shaped word in key 1 to be mistaken for one of ours.
  if (!have_version || version != PROTOCOL_VERSION) {
    return false;
  }
  if (!have_caps || caps == 0 || (caps & ~(uint32_t)CAP_ALL) != 0) {
    return false;
  }

  out->version = (uint8_t)version;
  out->caps = caps;
  out->graph_hours = graph_hours > 255 ? 255 : (uint8_t)graph_hours;
  return true;
}
