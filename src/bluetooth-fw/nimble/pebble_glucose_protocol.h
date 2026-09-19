// Pebble Glucose Protocol
//
// Generated from PROTOCOL.md. Do not edit directly.

#pragma once

#define PROTOCOL_VERSION 1

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
#define KEY_STATUS_START 17
#define KEY_STATUS_END 18
#define KEY_PUMP_CONNECTED 19  // uint8, 0=offline 1=connected; offline is the default
#define KEY_MEAL_CARBS 20      // uint16, grams of carbohydrate of the latest meal; omitted if none
#define KEY_MEAL_TIMESTAMP 21  // uint32, unix time the meal was recorded
#define KEY_PREDICTED_BG 22    // uint16, mg/dL predicted 30 minutes after the BG reading; omitted if none
// Keys 23-29 reserved

// Message keys: Sender -> watchface (raw graph)
#define KEY_GRAPH_DATA 30
#define KEY_GRAPH_HIGH_LINE 31
#define KEY_GRAPH_LOW_LINE 32
// Keys 33-39 reserved

// Keys 40-49 reserved for bitmap graph

// Capability bits
#define CAP_BG 0x01
#define CAP_TREND_ARROW 0x02
#define CAP_DELTA 0x04
#define CAP_IOB 0x08
#define CAP_STATUS 0x10
#define CAP_SENDER_BATTERY 0x20
#define CAP_PUMP_CONNECTED 0x40
#define CAP_MEAL 0x80
#define CAP_PREDICTION 0x100

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
