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
#define KEY_MEAL_TIMESTAMP 21  // uint32, unix time the sender learned of the meal (the pump's record carries no absolute time)
// Keys 22-29 reserved

// Message keys: Sender -> watchface (raw graph)
#define KEY_GRAPH_DATA 30
#define KEY_GRAPH_HIGH_LINE 31
#define KEY_GRAPH_LOW_LINE 32
// Keys 33-39 reserved

// Keys 40-49 reserved for bitmap graph

// Experimental keys, deliberately far outside the reserved ranges until they have soaked
#define KEY_PREDICTED_BG 1000  // uint16, mg/dL predicted 30 minutes after the BG reading; omitted if none
#define KEY_IOB_TOTAL_STRING 1001 // string "N.N" units: pump IOB plus the basal insulin still active; omitted if unknown
#define KEY_HYPO_TREAT_PCT 1002  // uint8, 0-100: treat-or-wait score for a falling low (see
                                 // sugar_predictor/INTEGRATION.md); omitted outside that regime
#define KEY_HYPO_P_LOW_PCT 1004 // uint8, 0-100: P(nadir < 70 in the next hour, untreated). Sent
                                 // alongside KEY_HYPO_TREAT_PCT as the confidence number for the
                                 // watchface's TREAT/WATCH decision -- treat_pct alone (which also
                                 // factors in overtreatment risk) reads as ambiguous on its own.
#define KEY_MEAL_LIST 1003      // blob: every meal still inside the graph window, not just the
                                 // latest. [count u8][(timestamp u32 LE)(grams u16 LE)] x count
#define KEY_SETTINGS_ALERTS 1005 // uint8, SETTINGS_ALERT_* bitmask (watchface -> sender): which
                                 // pump alert categories should pop up on the watch. Sent as part
                                 // of the capability announcement, so an old sender that doesn't
                                 // recognise the key just ignores it (ignores everything, in fact)
                                 // and keeps its own last-known/default value.
#define KEY_SETTINGS_FEATURES 1006 // uint8, SETTINGS_FEATURE_* bitmask (watchface -> sender): which
                                   // expensive-to-compute features to run at all, not just whether
                                   // to send/show the result -- CAP_* alone only silences the send.
                                   // Sent alongside KEY_SETTINGS_ALERTS; same ignore-if-unknown and
                                   // keep-last-value rules.

// Alert visibility bitmask for KEY_SETTINGS_ALERTS, watchface -> sender. Configured on the phone
// (the watchapp's Settings page); the sender falls back to SETTINGS_ALERT_LOW alone until the
// first announcement carries a value.
#define SETTINGS_ALERT_LOW 0x01   // predicted-low, low, and severe-low pump alerts
#define SETTINGS_ALERT_OTHER 0x02 // every other pump alert (reservoir, battery, sensor, SmartGuard, ...)

// Feature-enable bitmask for KEY_SETTINGS_FEATURES, watchface -> sender. Defaults to
// SETTINGS_FEATURE_HYPO on (the model's own decision, unchanged from before this key existed)
// until the first announcement says otherwise.
#define SETTINGS_FEATURE_HYPO 0x01 // run the hypo (treat-or-wait) model at all

// Capability bits
#define CAP_BG 0x01
#define CAP_TREND_ARROW 0x02
#define CAP_DELTA 0x04
#define CAP_IOB 0x08
#define CAP_STATUS 0x10
#define CAP_SENDER_BATTERY 0x20
#define CAP_PUMP_CONNECTED 0x40
#define CAP_MEAL 0x80
#define CAP_PREDICTION 0x10000 // experimental, like the keys above
#define CAP_IOB_TOTAL 0x20000
#define CAP_HYPO 0x40000       // watchface understands KEY_HYPO_TREAT_PCT
#define CAP_MEAL_LIST 0x80000  // watchface understands KEY_MEAL_LIST (else only the latest meal)

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
