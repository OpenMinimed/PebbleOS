/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Radio-less boards only (CONFIG_BT_HCI_FAKE): start the MiniMed task against a stand-in pump
//! link (minimed_fake_pump.c).
void minimed_fake_pump_start(void);
