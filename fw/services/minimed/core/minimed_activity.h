/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Insulin activity: forward-projected insulin action (U/h) from a ledger of deliveries. Pure --
//! no NimBLE/firmware dependency -- so the host harness (tools/minimed_sake_hosttest) can verify
//! it before it ever flashes. Design: INSULIN-ACTIVITY.md (issue #28).
//!
//! Deliveries come from reservoir differencing: a fall in reservoir remaining amount is insulin
//! that left the reservoir. Microboluses, boluses and autocorrections all appear in one stream.
//! The curve is the ledger convolved with a fixed kernel (Loop/oref exponential for Fiasp,
//! tp=55 / td=360 min), so it can be evaluated both at `now` and ahead of it -- the forward half
//! is the point (time a suspend or temp target against planned exercise).

#define MINIMED_ACTIVITY_LEDGER_MAX 256
#define MINIMED_ACTIVITY_STEP_SECS 300          //!< 5-minute grid
#define MINIMED_ACTIVITY_TP_MIN 55              //!< kernel peak
#define MINIMED_ACTIVITY_TD_MIN 360             //!< kernel duration
#define MINIMED_ACTIVITY_GAP_SECS 600           //!< a longer sample gap is spread as a flat rate
#define MINIMED_ACTIVITY_PRIME_SECS 300         //!< deltas discarded after a reservoir increase
#define MINIMED_ACTIVITY_PROVISIONAL_SECS (6 * 3600)  //!< ledger too young to be trusted

//! Serialized window: 3 h behind, 6 h ahead (= the kernel horizon) plus the point at now.
#define MINIMED_ACTIVITY_BACK_POINTS 36
#define MINIMED_ACTIVITY_FWD_POINTS 72
#define MINIMED_ACTIVITY_POINTS (MINIMED_ACTIVITY_BACK_POINTS + MINIMED_ACTIVITY_FWD_POINTS + 1)

typedef struct {
  uint32_t ts;  //!< unix seconds
  int32_t mu;   //!< insulin that left the reservoir (milli-IU)
} MinimedActivityEntry;

typedef struct {
  MinimedActivityEntry ledger[MINIMED_ACTIVITY_LEDGER_MAX];
  uint16_t head;  //!< next write index
  uint16_t count;
  int32_t last_res_mu;
  bool have_res;
  uint32_t last_sample_ts;
  uint32_t first_sample_ts;
  uint32_t prime_until;        //!< discard delivery deltas until this time
  uint32_t provisional_until;  //!< mark the curve provisional until this time
} MinimedActivity;

void minimed_activity_init(MinimedActivity *a);

//! Feed one reservoir sample (milli-IU). `res_mu <= 0` means unknown (seen while the reservoir is
//! detached during a set change), not empty: the sample is ignored and the baseline kept. Returns
//! true if a delivery event was recorded. A level increase is a refill/prime: it opens a
//! PRIME_SECS window in which deltas are discarded, and never clears the ledger (insulin already
//! delivered is still in the body).
bool minimed_activity_observe_reservoir(MinimedActivity *a, int32_t res_mu, uint32_t now);

//! Insulin action at `t`, in milli-IU per minute. Sum over the ledger of d * k(t - ts).
float minimed_activity_eval_mu_per_min(const MinimedActivity *a, uint32_t t);

//! Serialize the sampled curve in the KEY_GRAPH_DATA wire layout (little-endian):
//! [ref_ts u32][count u16][offset_min u16 xN][activity u8 xN], where activity is 0.05 U/h per
//! LSB (12.75 U/h ceiling). The points sit on the 5-minute grid ending at `now` + 6 h, so the
//! point at index MINIMED_ACTIVITY_BACK_POINTS is exactly now and the rest are the forward
//! projection. Returns the byte count, or 0 with no ledger (nothing real to draw -- omit the
//! key, as with the BG graph).
#define MINIMED_ACTIVITY_BLOB_MAX (6 + 3 * MINIMED_ACTIVITY_POINTS)
uint16_t minimed_activity_serialize(const MinimedActivity *a, uint32_t now, uint8_t *out);

//! True while the curve should be marked provisional: either the ledger is younger than 6 h, or a
//! gap newer than the kernel duration has not yet aged out.
bool minimed_activity_provisional(const MinimedActivity *a, uint32_t now);
