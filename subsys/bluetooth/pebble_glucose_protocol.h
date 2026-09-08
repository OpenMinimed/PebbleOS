// Pebble Glucose Protocol
//
// Copy this file into your watchface/sender project.
// Port to other languages as needed.
//
// See PROTOCOL.md for all definitions.

#pragma once

#define PROTOCOL_VERSION 1  // draft!

// Message keys: Watchface -> sender (capability announcement)
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1
#define KEY_GRAPH_HOURS 2
// Keys 3-9 reserved

// Message keys: Sender -> watchface (data)
#define KEY_BG_TIMESTAMP 10
#define KEY_BG_STRING 11
#define KEY_DELTA_STRING 12
#define KEY_TREND_ARROW 13
#define KEY_IOB_STRING 14
#define KEY_STATUS_STRING 15
#define KEY_SENDER_BATTERY 16
// Keys 17-29 reserved

// Message keys: Sender -> watchface (raw graph)
#define KEY_GRAPH_DATA 30
#define KEY_GRAPH_HIGH_LINE 31
#define KEY_GRAPH_LOW_LINE 32
// Keys 33-39 reserved

// Keys 40-49 reserved for bitmap graph

// Capability bits
#define CAP_BG (1 << 0)
#define CAP_TREND_ARROW (1 << 1)
#define CAP_DELTA (1 << 2)
#define CAP_IOB (1 << 3)
#define CAP_STATUS (1 << 4)
#define CAP_SENDER_BATTERY (1 << 5)

// Trend arrow indices
#define TREND_UNKNOWN 0
#define TREND_FLAT 1
#define TREND_SLANT_UP 2
#define TREND_SLANT_DOWN 3
#define TREND_UP 4
#define TREND_DOWN 5
#define TREND_DOUBLE_UP 6
#define TREND_DOUBLE_DOWN 7
#define TREND_TRIPLE_UP 8
#define TREND_TRIPLE_DOWN 9
