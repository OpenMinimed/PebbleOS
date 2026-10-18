/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "minimed_predict.h"

//! Run the hypo model on the predictor's window, and send the result to the watchface and, past
//! the treat threshold, as an alert popup. `bg_str` is the BG shown in the popup.
void minimed_hypo_check(const MinimedPredictWindow *win, const char *bg_str);
