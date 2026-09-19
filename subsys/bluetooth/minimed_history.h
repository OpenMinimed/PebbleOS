/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Pure decode of IDD History Data records other than annunciations (those live in
//! minimed_annunciation.{c,h}). NimBLE-free so the host harness links it. Input is one
//! reassembled, decrypted record: event type(2 LE) | sequence number(4 LE) | relative offset(2 LE)
//! | event data. Formats: Documentation/idd-service.md + PythonPumpConnector history/events.

//! NGP Reference Time (0xf00e): the pump's clock, logged about hourly. The pump's history has no
//! absolute times of its own: every other record's time is relative to the latest reference time
//! before it.
typedef struct {
  uint32_t seq;
  uint32_t secs;  //!< pump clock as seconds since 2000-01-01 (local time, no time zone; only
                  //!< differences between values are meaningful)
} MinimedHistRef;

//! Decode a Reference Time record. False if it is another event type, truncated, or holds an
//! impossible date.
bool minimed_history_parse_ref_time(const uint8_t *rec, uint16_t len, MinimedHistRef *out);

//! SG Measurement (0xf00c): one CGM sample as stored in the pump's event log.
typedef struct {
  uint32_t seq;
  int16_t offset_min;  //!< minutes after the governing Reference Time (not the CGM session clock)
  uint16_t sg;         //!< raw SG value; see minimed_history_sg_to_mgdl
} MinimedHistSg;

//! Pump-clock time of a sample: its Reference Time plus its offset. The offset, not the record's
//! own Relative Offset, is used: samples the sensor logged late (after "sensor updating") share a
//! log time but keep their own sample time in the offset.
uint32_t minimed_history_sg_secs(const MinimedHistRef *ref, const MinimedHistSg *sg);

//! Decode an SG Measurement record. False if it is another event type or is truncated.
bool minimed_history_parse_sg(const uint8_t *rec, uint16_t len, MinimedHistSg *out);

//! Meal (0xf005): carbohydrates entered for a meal bolus.
typedef struct {
  uint32_t seq;
  uint16_t grams;  //!< food amount, rounded to whole grams
} MinimedHistMeal;

//! Decode a Meal record. False if it is another event type, truncated, or carries no usable amount
//! (a MedFloat16 NaN/reserved value or a negative amount).
bool minimed_history_parse_meal(const uint8_t *rec, uint16_t len, MinimedHistMeal *out);

//! Insulin the pump logged as delivered. The pump has no single "insulin delivered" event: a bolus
//! is one record, closed-loop basal is a stream of small microboluses, and manual-mode basal is a
//! rate that holds until the next rate change.
typedef enum {
  MinimedHistInsulinBolus,      //!< Bolus Delivered Part 1 (0x0069): fast + extended amount, U
  MinimedHistInsulinMicro,      //!< Auto Basal Delivery (0xf001): one microbolus, U
  MinimedHistInsulinBasalRate,  //!< Delivered Basal Rate Changed (0x0099): the new rate, U/h
} MinimedHistInsulinKind;

typedef struct {
  uint32_t seq;
  MinimedHistInsulinKind kind;
  float amount;       //!< U for a bolus or microbolus, U/h for a rate
  bool by_algorithm;  //!< BasalRate only: set by the pump's algorithm rather than a profile; its
                      //!< delivery then arrives as microboluses instead
} MinimedHistInsulin;

//! Decode a bolus, microbolus or basal-rate record. False for any other type or a short record.
bool minimed_history_parse_insulin(const uint8_t *rec, uint16_t len, MinimedHistInsulin *out);

//! Special SG Value codes (Documentation/idd-service.md): no sample, or clamped at the sensor's
//! range edge.
#define MINIMED_HIST_SG_STARTING 0x0301
#define MINIMED_HIST_SG_UPDATING 0x0303
#define MINIMED_HIST_SG_ABOVE 0x0308
#define MINIMED_HIST_SG_BELOW 0x030d

//! Convert a raw SG value to mg/dL. Off-scale codes map to `floor_mgdl` / `ceiling_mgdl` (where
//! the caller graphs "at or beyond" readings). Returns -1 for "no usable sample" (starting,
//! updating, or any other code in the special range).
int32_t minimed_history_sg_to_mgdl(uint16_t sg, int32_t floor_mgdl, int32_t ceiling_mgdl);

//! One history record reduced to what the predictor needs, with its time on the pump's clock.
typedef enum {
  MinimedHistEventRef,      //!< a Reference Time: updates the clock, carries no data
  MinimedHistEventSg,       //!< a CGM sample: `mgdl` (-1 if no usable value), `sg` raw code
  MinimedHistEventInsulin,  //!< a bolus or microbolus delivered: `value` U
  MinimedHistEventBasal,    //!< a basal rate from now on: `value` U/h, 0 when the algorithm sets it
  MinimedHistEventCarbs,    //!< a meal: `value` grams (never 0)
} MinimedHistEventKind;

typedef struct {
  MinimedHistEventKind kind;
  uint32_t seq;
  uint32_t secs;  //!< pump clock, as MinimedHistRef.secs
  float value;
  int32_t mgdl;
  uint16_t sg;
} MinimedHistEvent;

//! Follows the log so records can be given a time: the latest Reference Time before them.
typedef struct {
  bool have_ref;
  MinimedHistRef ref;
} MinimedHistClock;

//! Decode one record, given in log order. Reference Time records update `clk`; every other record
//! is timed from it: a sample by its own minute offset, anything else by the record's Relative
//! Offset (seconds since the Reference Time). False for a record of no interest, a malformed one,
//! or one seen before any Reference Time (its time is unknown). `floor_mgdl`/`ceiling_mgdl` are
//! where off-scale samples are plotted.
bool minimed_history_decode(MinimedHistClock *clk, const uint8_t *rec, uint16_t len,
                            int32_t floor_mgdl, int32_t ceiling_mgdl, MinimedHistEvent *out);
