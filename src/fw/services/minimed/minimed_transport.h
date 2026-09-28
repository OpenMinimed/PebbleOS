/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! The session's view of the pump link: GATT client operations on named characteristics, with
//! results delivered as MinimedEvents on the MiniMed task (minimed_task.h). The firmware
//! implementation is src/bluetooth-fw/nimble/minimed_transport_nimble.c. A watchapp port
//! implements the same calls with the SDK's ble_client_* API (read, write, subscribe, read and
//! notify handlers, all keyed by characteristic), so the session above it moves over unchanged.
//!
//! What is deliberately NOT here: advertising as a pump peripheral, the SAKE GATT server and its
//! handshake, and the pairing window. Those are the parts the SDK has no API for yet, and they stay
//! in the driver (minimed_sake_service.c).

//! Every characteristic the session uses. The first group is found by service discovery; the
//! second is read by UUID over the whole handle range and needs none.
typedef enum {
  MinimedChrCgmMeasurement,    //!< CGM 0x2AA7, notify, SAKE-encrypted
  MinimedChrCgmFeature,        //!< CGM 0x2AA8, read, plaintext
  MinimedChrCgmRacp,           //!< CGM 0x2A52, write + indicate, plaintext
  MinimedChrCgmSessionStart,   //!< CGM 0x2AAA, read
  MinimedChrSensorExpiration,  //!< Medtronic 0x0202, indicate
  MinimedChrIddSrcp,           //!< IDD 0x0105, write + indicate, SAKE-encrypted
  MinimedChrIddStatusChanged,  //!< IDD 0x0101, indicate, SAKE-encrypted
  MinimedChrIddStatus,         //!< IDD 0x0102, read, SAKE-encrypted
  MinimedChrIddRacp,           //!< IDD 0x2A52, write + indicate, plaintext
  MinimedChrIddHistory,        //!< IDD 0x0108, notify, SAKE-encrypted per fragment
  MinimedChrIddAnnuncStatus,   //!< IDD 0x0103, read, SAKE-encrypted: the annunciation shown now
  MinimedChrIddCommandCp,      //!< IDD 0x0106, write + indicate, SAKE-encrypted
  MinimedChrIddCommandData,    //!< IDD 0x0107, notify, SAKE-encrypted

  MinimedChrBattery,           //!< 0x2A19
  MinimedChrSessionRunTime,    //!< 0x2AAB
  MinimedChrIddFeatures,       //!< Medtronic 0x0104
  MinimedChrCurrentTime,       //!< Current Time 0x2A2B, the pump clock
  MinimedChrDisManufacturer,   //!< Device Information 0x2A29
  MinimedChrDisModel,          //!< 0x2A24
  MinimedChrDisSerial,         //!< 0x2A25
  MinimedChrDisHardware,       //!< 0x2A27
  MinimedChrDisFirmware,       //!< 0x2A26
  MinimedChrDisSoftware,       //!< 0x2A28
  MinimedChrDisSystemId,       //!< 0x2A23
  MinimedChrDisPnpId,          //!< 0x2A50
  MinimedChrDisRegulatory,     //!< 0x2A2A

  MinimedChrCount
} MinimedChr;

#define MINIMED_CHR_FIRST_BY_UUID MinimedChrBattery

typedef struct {
  bool ok;
  bool link_dead;  //!< the error means the link itself is gone, not just this operation
  uint16_t code;   //!< raw stack status, for logs
} MinimedGattStatus;

//! Find the CGM and IDD services and their characteristics. Posts MinimedEventDiscovered once
//! done, whatever was found; minimed_transport_has says what.
void minimed_transport_discover(void);

//! True if `chr` can be used: discovered, or one of the read-by-UUID ones while the link is up.
bool minimed_transport_has(MinimedChr chr);

//! Read `chr`. Posts MinimedEventGattDone with `tag` and the value (the first match, for a
//! read-by-UUID characteristic). A false return means the read never started; no event follows.
bool minimed_transport_read(MinimedChr chr, uint8_t tag, MinimedGattStatus *status);

//! Write `chr` with response. Posts MinimedEventGattDone with `tag` when the response arrives,
//! or, with `tag` 0, posts nothing. False: never started, no event.
bool minimed_transport_write(MinimedChr chr, const uint8_t *data, uint16_t len, uint8_t tag,
                             MinimedGattStatus *status);

//! Enable notifications (`indicate` false) or indications on `chr`. Completes like a write.
bool minimed_transport_subscribe(MinimedChr chr, bool indicate, uint8_t tag,
                                 MinimedGattStatus *status);

//! Largest value one notification carries on this link (ATT MTU - 3). The pump fills history
//! fragments to it, so a shorter fragment ends a record.
uint16_t minimed_transport_max_value_len(void);

//! Whether the stack still has the connection, and its MTU: a diagnostic for a silent pump.
bool minimed_transport_link_alive(uint16_t *mtu);

//! SAKE session cipher from the handshake (minimed_sake_service.c): decrypt pump -> watch values,
//! encrypt watch -> pump requests. False until the handshake has completed.
bool minimed_sake_decrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t out_cap,
                          uint16_t *out_len);
bool minimed_sake_encrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t *out_len);
