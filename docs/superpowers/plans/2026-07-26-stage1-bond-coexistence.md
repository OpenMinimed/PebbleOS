# Stage 1: Phone + Pump Bond Coexistence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop non-gateway (MiniMed pump) BLE bonds being destroyed by machinery that assumes every BLE bond is the phone, so neither the pump nor the phone ever has to be paired again after a firmware flash.

**Architecture:** One idea applied at three sites in the Pebble firmware's bond storage and Bluetooth settings, plus a live diagnostic readout so a failure is legible on the watch without a phone. All three functional edits are no-ops in stock builds, where every BLE bond is a gateway (`is_gateway == true`). No BLE protocol behaviour changes; nothing about the pump link, SAKE, or advertising is touched.

**Tech Stack:** C (PebbleOS firmware), waf build system inside Docker, clar unit-test harness, board `asterix` (Pebble 2 Duo, nRF52840, NimBLE).

**Spec:** `docs/superpowers/specs/2026-07-26-dual-connection-design.md`, Stage 1 (sections 1.1–1.4). Stage 2 (dual connection) is explicitly **not** in this plan.

## Global Constraints

- Branch: `spike/minimed-sake`. Do not merge, rebase, or push — Morten pushes.
- Commit with `-s` (Signed-off-by is enforced by gitlint's `contrib-body-requires-signed-off-by`).
- Commit title format `area: short description`, lowercase after the colon, max 100 chars. Verify with `uvx --from gitlint-core gitlint --commits HEAD~1..HEAD` (gitlint is not installed locally; `uvx` is).
- Every commit must include the co-author trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Commit in small chunks, preserving bisectability. One task = one commit.
- Code comments explain **why**, not what. Do not reference issue numbers in code — those go in the commit body only.
- clang-format for C. Keep comments short; put extended reasoning in the commit message.
- **All builds and tests run inside Docker.** There is no local Python toolchain (`intelhex` is missing, so bare `./waf` fails).
- **Never run `./waf configure` without `--board`.** It wipes `build/c4che` and de-configures the firmware build. Recovery is in the Snags section below.
- This is read-only DIY-diabetes tooling. Do not add any therapeutic write capability to the pump driver under any circumstances. Nothing in this plan goes near it.

## Snags and Exact Commands (read before Task 1)

These were established empirically on 2026-07-26. Use them verbatim.

**Run the unit tests:**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected on success, as the last lines:

```
test summary
  tests that pass 2/2
    /pebbleos/build/test/tests/fw/services/bluetooth/test_bluetooth_persistent_storage_prf_obelix/runme_obelix
    /pebbleos/build/test/tests/fw/services/bluetooth/test_bluetooth_persistent_storage/runme
```

Notes that will otherwise cost you time:

- The `-M` filter is `re.match` (anchored at the start) against the test source path, so a bare
  `-M bluetooth_persistent_storage` silently matches **nothing** and the run "succeeds" having built
  no tests. Always wrap it: `-M '.*name.*'`.
- `./waf test` builds into the `test` **variant** (`build/test/`), so it does not clobber the
  firmware build. Running it with the asterix configure in place is fine and expected.
- The first test run compiles ~2200 targets and takes roughly 2 minutes. Later runs are ~1 second.
- Run the full suite (drop `-M`) before the final commit of the last task.

**Build the firmware** (only needed once, at the end, to produce a flashable bundle):

```bash
./spike-build.sh bond-coexistence
```

That wraps the Docker invocation, auto-increments the version to v36, and pushes the `.pbz` to the
phone. It needs the pump link **not** to be in the way: put the watch in NORMAL mode first.

**If you wiped the build config** (symptom: `build/c4che` empty, or waf complains about no board):

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH
    ./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y"
```

Verify with `grep -o 'CONFIG_MINIMED_SAKE_SPIKE = .*' build/c4che/_cache.py` (expect `True`) and
`grep MINIMED build/autoconf.h` (expect `#define CONFIG_MINIMED_SAKE_SPIKE 1`).

## Background: why each edit is needed

`BtPersistBondingBLEData` (`bluetooth_persistent_storage_normal.c:53-60`) has an `is_gateway` bit.
The phone's bond has it set; the pump's bond does not, because `nimble_store.c:258-260` clears it
for any bond formed while the watch is in SPIKE mode. Three places treat every BLE bond as if it
were the phone's:

1. `prv_collect_other_ble_bondings_itr` gathers "every other BLE bond" for deletion with no
   `is_gateway` check. Both boot-time prune paths and the phone-re-pair prune funnel through it, so
   it is why the pump bond never survives a reboot.
2. `bt_persistent_storage_delete_ble_pairing_by_id` erases the shared-PRF pairing slot on any
   delete, so forgetting the *pump* wipes the *phone's* PRF record.
3. Settings → Bluetooth enables pairability only when the bonded-device list is empty, and builds
   that list from every BLE bond. Once the pump bond survives, you could never pair a phone again.

Fixing (1) and (2) without (3) leaves the watch unable to pair a phone. All three ship together.

## File Structure

| File | Change | Responsibility |
| --- | --- | --- |
| `src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c` | Modify | Bond storage. Tasks 1, 2 and the counter half of Task 4. |
| `include/pbl/services/bluetooth/bluetooth_persistent_storage.h` | Modify | Public declaration of the Task 4 diagnostic accessor. |
| `src/fw/apps/system/settings/bluetooth.c` | Modify | Task 3: hide non-gateway bonds from the phone list. |
| `src/fw/apps/system/minimed_sake_app.c` | Modify | Task 4: render the bond inventory line. |
| `tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c` | Modify | Unit tests for Tasks 1, 2, 4. Existing file, 727 lines, append new tests at the end. |

Tasks 1, 2 and 4 are covered by host unit tests. Task 3 is a UI change with no unit-test harness in
this repo (`tests/fw/apps/` has no settings tests); it is verified on hardware — this is called out
honestly in that task rather than papered over with a fake test.

---

### Task 1: The prune collector skips non-gateway bonds

This is the core fix. It closes three holes at once, because the phone-re-pair prune, the boot SPRF
replay prune and `prv_prune_stale_ble_bondings` all reach the pump through this one iterator.

**Files:**
- Modify: `src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c:597-621` (`prv_collect_other_ble_bondings_itr`) and the comment on `prv_delete_other_ble_bondings` at `:623-629`
- Test: `tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c` (append)

**Interfaces:**
- Consumes: nothing from earlier tasks (first task).
- Produces: the behavioural guarantee later tasks rely on — after a gateway bond is stored, any
  non-gateway BLE bond still exists. No new function signatures.

- [ ] **Step 1: Write the failing test**

Append to `tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c`. Note the existing file
already uses this exact `SMPairingInfo` construction style — follow it.

```c
// A non-gateway bond (the MiniMed pump) must survive a gateway (phone) bond being stored. Before
// this fix the prune deleted every other BLE bond unconditionally, so pairing the phone destroyed
// the pump bond and the pump had to be re-paired after every flash.
void test_bluetooth_persistent_storage__non_gateway_survives_gateway_store(void) {
  SMPairingInfo pump = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xaa, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xaa, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xaa, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = true,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID pump_id = bt_persistent_storage_store_ble_pairing(&pump, false /* is_gateway */,
                                                                NULL,
                                                                false /* requires_address_pinning */,
                                                                false /* auto_accept_re_pairing */);
  cl_assert(pump_id != BT_BONDING_ID_INVALID);

  SMPairingInfo phone = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xbb, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xbb, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xbb, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID phone_id = bt_persistent_storage_store_ble_pairing(&phone, true /* is_gateway */,
                                                                 NULL,
                                                                 false /* requires_address_pinning */,
                                                                 false /* auto_accept_re_pairing */);
  cl_assert(phone_id != BT_BONDING_ID_INVALID);

  // Both must still be there.
  cl_assert(bt_persistent_storage_get_ble_pairing_by_id(pump_id, NULL, NULL, NULL));
  cl_assert(bt_persistent_storage_get_ble_pairing_by_id(phone_id, NULL, NULL, NULL));
}

// The other direction: storing a second gateway still evicts the first. Stock single-phone policy
// must be unchanged -- this fix must not turn the watch into a multi-phone device.
void test_bluetooth_persistent_storage__gateway_still_evicts_gateway(void) {
  SMPairingInfo phone_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xc1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xc1, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xc1, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_1 = bt_persistent_storage_store_ble_pairing(&phone_1, true /* is_gateway */, NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);

  SMPairingInfo phone_2 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xc2, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xc2, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xc2, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_2 = bt_persistent_storage_store_ble_pairing(&phone_2, true /* is_gateway */, NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);

  cl_assert(!bt_persistent_storage_get_ble_pairing_by_id(id_1, NULL, NULL, NULL));
  cl_assert(bt_persistent_storage_get_ble_pairing_by_id(id_2, NULL, NULL, NULL));
}
```

- [ ] **Step 2: Run the tests to verify the first one fails**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `test_bluetooth_persistent_storage__non_gateway_survives_gateway_store` FAILS (the pump
bond was pruned, so `get_ble_pairing_by_id(pump_id, ...)` returns false).
`test_bluetooth_persistent_storage__gateway_still_evicts_gateway` should already PASS — it pins
existing behaviour so the next step cannot break it.

If the first test *passes* at this point, stop: either the fix is already applied or the test is not
exercising the prune. Do not proceed.

- [ ] **Step 3: Write the implementation**

In `prv_collect_other_ble_bondings_itr`, after the existing `stored_data.type` check, skip
non-gateway bonds:

```c
  BtPersistBondingData stored_data;
  info->get_val(file, (uint8_t *)&stored_data, MIN((unsigned)info->val_len, sizeof(stored_data)));
  if (stored_data.type != BtPersistBondingTypeBLE) {
    return true;
  }

  // Never prune a NON-gateway bond (the MiniMed pump). The single-BLE-pairing policy is about
  // gateways -- one phone -- and the pump is not competing for that role. Pruning it here is what
  // deleted the pump bond on every reboot (the boot SPRF replay re-stores the phone as a gateway,
  // which lands in this collector) and on every phone re-pair, forcing a pump re-pair each time.
  // In stock builds every BLE bond is a gateway, so this skip never fires.
  if (!stored_data.ble_data.is_gateway) {
    return true;
  }
```

Then correct the now-inaccurate doc comment on `prv_delete_other_ble_bondings`:

```c
//! Delete every other BLE *gateway* bonding except `keep_id`. We only ever support one BLE gateway
//! (phone) pairing at a time, so any other gateway bonding present is stale and must be removed
//! (e.g. when a new phone pairs and replaces the previous one). Non-gateway bondings (the MiniMed
//! pump) are deliberately left alone -- see prv_collect_other_ble_bondings_itr.
//!
//! Uses the internal delete helper that does not erase shared PRF pairing data, since the kept
//! entry is the one that should remain reflected in PRF storage.
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `tests that pass 2/2`, with both new tests passing and no previously-passing test
regressing. In particular `test_bluetooth_persistent_storage__ble_store_and_get` and
`test_bluetooth_persistent_storage__delete_all` must still pass.

- [ ] **Step 5: Commit**

```bash
git add src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c \
        tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c
git commit -s -m "bluetooth: never prune non-gateway BLE bonds

The single-BLE-pairing prune collected every other BLE bond for
deletion with no is_gateway check, so the MiniMed pump's non-gateway
bond was destroyed whenever a gateway bond was written. That happens
on every phone re-pair and, crucially, on every boot -- the SPRF
replay in bt_persistent_storage_init re-stores the phone as a gateway,
which lands in this same collector. Hence a pump re-pair after every
single firmware flash.

The policy is about gateways: one phone. The pump does not compete for
that role, so skip non-gateway bonds in the collector. This also
defuses prv_prune_stale_ble_bondings, which deletes through the same
path. v33 fixed the mirror image (a non-gateway write evicting the
phone); this completes the pair.

No-op in stock builds, where every BLE bond is a gateway.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
uvx --from gitlint-core gitlint --commits HEAD~1..HEAD
```

Expected: gitlint produces no output and exits 0.

---

### Task 2: Deleting a non-gateway bond must not erase the phone's PRF slot

**Files:**
- Modify: `src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c:908-914` (`bt_persistent_storage_delete_ble_pairing_by_id`)
- Test: `tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c` (append)

**Interfaces:**
- Consumes: Task 1's guarantee that a non-gateway bond can coexist with a gateway bond (the test
  needs both present at once).
- Produces: no new signatures. Behavioural guarantee: `bt_persistent_storage_delete_ble_pairing_by_id`
  calls `shared_prf_storage_erase_ble_pairing_data()` only for gateway bonds.

The test asserts on `fake_shared_prf_storage_get_ble_delete_count()`, which already exists in
`tests/fakes/fake_shared_prf_storage.h` and is already included by this test file.

- [ ] **Step 1: Write the failing test**

```c
// Forgetting the PUMP must not wipe the PHONE's shared-PRF pairing slot. The erase was
// unconditional, so pressing DOWN (forget pump) on the watch -- a routine step in the FE81/FE82
// reconciliation -- left the phone unpaired in PRF until the next boot repaired it.
void test_bluetooth_persistent_storage__deleting_non_gateway_keeps_prf(void) {
  SMPairingInfo pump = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xd1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xd1, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xd1, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = true,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID pump_id = bt_persistent_storage_store_ble_pairing(&pump, false /* is_gateway */,
                                                                NULL,
                                                                false /* requires_address_pinning */,
                                                                false /* auto_accept_re_pairing */);

  fake_shared_prf_storage_reset_counts();
  bt_persistent_storage_delete_ble_pairing_by_id(pump_id);

  cl_assert(!bt_persistent_storage_get_ble_pairing_by_id(pump_id, NULL, NULL, NULL));
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 0);
}

// Deleting the phone must still erase the PRF slot -- stock behaviour, unchanged.
void test_bluetooth_persistent_storage__deleting_gateway_erases_prf(void) {
  SMPairingInfo phone = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xd2, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xd2, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xd2, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID phone_id = bt_persistent_storage_store_ble_pairing(&phone, true /* is_gateway */,
                                                                 NULL,
                                                                 false /* requires_address_pinning */,
                                                                 false /* auto_accept_re_pairing */);

  fake_shared_prf_storage_reset_counts();
  bt_persistent_storage_delete_ble_pairing_by_id(phone_id);

  cl_assert(!bt_persistent_storage_get_ble_pairing_by_id(phone_id, NULL, NULL, NULL));
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 1);
}
```

- [ ] **Step 2: Run the tests to verify the first one fails**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `..._deleting_non_gateway_keeps_prf` FAILS with the delete count being 1, not 0.
`..._deleting_gateway_erases_prf` should already PASS.

- [ ] **Step 3: Write the implementation**

Replace `bt_persistent_storage_delete_ble_pairing_by_id` with:

```c
void bt_persistent_storage_delete_ble_pairing_by_id(BTBondingID bonding) {
  // Read this BEFORE the delete -- the record is gone afterwards. supports_ancs is set equal to
  // is_gateway when a bond is stored, so this answers "was this the phone?".
  const bool was_gateway = bt_persistent_storage_is_ble_ancs_bonding(bonding);

  if (!prv_delete_ble_pairing_by_id(bonding)) {
    return;
  }

  // Only the gateway (phone) is mirrored into shared PRF storage, so only its deletion should clear
  // that slot. Erasing on a non-gateway delete wiped the phone's PRF pairing when the pump was
  // forgotten -- self-healing at the next boot, but leaving PRF/recovery unpaired until then.
  if (was_gateway) {
    shared_prf_storage_erase_ble_pairing_data();
  }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `tests that pass 2/2`, all four new tests from Tasks 1 and 2 passing.

- [ ] **Step 5: Commit**

```bash
git add src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c \
        tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c
git commit -s -m "bluetooth: only erase the PRF pairing slot for gateway bonds

bt_persistent_storage_delete_ble_pairing_by_id erased the shared-PRF
pairing data on any successful delete, so forgetting the MiniMed pump
wiped the phone's PRF slot. It self-heals at the next boot, but until
then a crash into PRF or recovery finds no phone pairing -- and
forgetting the pump (DOWN on the watch) is a routine step in the
FE81/FE82 reconciliation procedure.

Only the gateway is mirrored into PRF storage, so gate the erase on
the bond having been a gateway, captured before the delete because
the record is gone afterwards.

No-op in stock builds, where every BLE bond is a gateway.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
uvx --from gitlint-core gitlint --commits HEAD~1..HEAD
```

---

### Task 3: Settings → Bluetooth ignores non-gateway bonds

**This task is not optional and must ship with Tasks 1 and 2.** It fixes a trap those tasks create:
once the pump bond survives permanently, Settings would refuse to pair a phone ever again.

**Files:**
- Modify: `src/fw/apps/system/settings/bluetooth.c` (`prv_add_ble_remote`)

**Interfaces:**
- Consumes: `bt_persistent_storage_is_ble_ancs_bonding(BTBondingID)` — already declared in
  `include/pbl/services/bluetooth/bluetooth_persistent_storage.h:74`, returns `true` for gateway
  bonds. No signature changes anywhere.
- Produces: nothing consumed by later tasks.

**Why there is no unit test here:** this repo has no test harness for the settings apps
(`tests/fw/` contains no settings-app tests, only `tests/fw/services/settings/test_settings_file.c`
which covers the storage layer). Writing a fake window/menu harness for a two-line guard would cost
far more than it proves. Verification is hardware check 3 in the Validation section. Do not invent a
test that only exercises `bt_persistent_storage_is_ble_ancs_bonding` — that function is already
covered by `test_bluetooth_persistent_storage__ble_ancs_bonding`, and a test that doesn't exercise
`prv_add_ble_remote` would give false confidence.

- [ ] **Step 1: Read the current function**

Read `prv_add_ble_remote` in `src/fw/apps/system/settings/bluetooth.c`. It currently reads:

```c
static void prv_add_ble_remote(BTDeviceInternal *device, SMIdentityResolvingKey *irk,
                               const char *name, BTBondingID *id, void *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData*) context;
  if (!data) {
    return;
  }

  StoredRemote* remote = stored_remote_create();
  remote->ble.bonding = *id;
  prv_copy_device_name_with_fallback(remote, name);
  add_remote(data, remote);
}
```

Confirm it matches before editing. If it does not, stop and report — the plan is stale.

- [ ] **Step 2: Write the implementation**

```c
static void prv_add_ble_remote(BTDeviceInternal *device, SMIdentityResolvingKey *irk,
                               const char *name, BTBondingID *id, void *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData*) context;
  if (!data) {
    return;
  }

  // This menu is the "paired phone" list, and pairability is enabled only while it is empty (see
  // prv_expand_cb). A non-gateway bond -- the MiniMed pump -- is not a phone: listing it would
  // both miscount the header and permanently block pairing a new phone, since the pump bond now
  // survives reboots. In stock builds every BLE bond is a gateway, so nothing is ever skipped.
  if (!bt_persistent_storage_is_ble_ancs_bonding(*id)) {
    return;
  }

  StoredRemote* remote = stored_remote_create();
  remote->ble.bonding = *id;
  prv_copy_device_name_with_fallback(remote, name);
  add_remote(data, remote);
}
```

- [ ] **Step 3: Verify the header is already included**

Run:

```bash
grep -n 'bluetooth_persistent_storage.h' src/fw/apps/system/settings/bluetooth.c
```

Expected: a match. The file already calls `bt_persistent_storage_get_ble_pairing_by_id`, so the
header is present. If there is no match, add
`#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"` with the other includes.

- [ ] **Step 4: Verify it compiles**

The settings app is firmware-only, so build the firmware rather than the tests:

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH
    ./waf build" 2>&1 | tail -15
```

Expected: `'build' finished successfully`. A compile error naming `prv_add_ble_remote` or
`bt_persistent_storage_is_ble_ancs_bonding` means the include is missing — go back to Step 3.

- [ ] **Step 5: Run the unit tests to confirm nothing regressed**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `tests that pass 2/2`.

- [ ] **Step 6: Commit**

```bash
git add src/fw/apps/system/settings/bluetooth.c
git commit -s -m "bluetooth: hide non-gateway bonds from the settings phone list

Settings -> Bluetooth enables pairability only while the remote list
is empty, and built that list from every stored BLE bond. Now that
the pump's non-gateway bond survives reboots, it would sit in that
list forever: the menu would show it as a paired phone, count it in
the header, and permanently refuse to pair a new phone behind
\"Forget this device to pair a new device\".

Skip non-gateway bonds when building the list. supports_ancs tracks
is_gateway, so the existing public accessor answers this without
changing the BtPersistBondingDBEachBLE callback signature.

No-op in stock builds, where every BLE bond is a gateway.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
uvx --from gitlint-core gitlint --commits HEAD~1..HEAD
```

---

### Task 4: Live bond-inventory readout in the SAKE Spike app

Makes a Stage 1 failure legible on the watch with no phone attached — which matters because if
Stage 1 fails, the pump link is exactly what is broken, so the phone-log channel is unavailable by
definition.

The bond pruning runs in `services_common_init` long before the BT stack, and `minimed_sake_log`
needs `kernel_malloc` plus the launcher task, so boot-time logging is not viable. The 8-line ring
also scrolls, so a boot line would be gone before it could be read. Hence **live state, not
history**, rendered by the app's existing 400 ms refresh.

**Files:**
- Modify: `src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c` (counting iterator, delete counter, new accessor)
- Modify: `include/pbl/services/bluetooth/bluetooth_persistent_storage.h` (declare the accessor)
- Modify: `src/fw/apps/system/minimed_sake_app.c` (`prv_refresh`)
- Test: `tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c` (append)

**Interfaces:**
- Consumes: Tasks 1 and 2's behaviour (the test stores a gateway and a non-gateway bond and expects
  both to survive).
- Produces:
  `void bt_persistent_storage_get_ble_bonding_counts(uint8_t *gateway_out, uint8_t *non_gateway_out, uint8_t *non_gateway_deleted_out);`
  — counts stored BLE bonds by gateway status, plus a since-boot counter of non-gateway deletions.
  Any out-param may be NULL. This is the only new public symbol in Stage 1.

- [ ] **Step 1: Write the failing test**

```c
// The bond inventory is the on-watch diagnostic for Stage 1: it distinguishes "the pump bond was
// pruned" from "the pump bond was never stored", which otherwise look identical on the watch.
void test_bluetooth_persistent_storage__ble_bonding_counts(void) {
  uint8_t gateway = 0xff, non_gateway = 0xff, deleted = 0xff;

  bt_persistent_storage_get_ble_bonding_counts(&gateway, &non_gateway, &deleted);
  cl_assert_equal_i(gateway, 0);
  cl_assert_equal_i(non_gateway, 0);
  cl_assert_equal_i(deleted, 0);

  SMPairingInfo pump = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xe1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xe1, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xe1, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = true,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID pump_id = bt_persistent_storage_store_ble_pairing(&pump, false /* is_gateway */,
                                                                NULL,
                                                                false /* requires_address_pinning */,
                                                                false /* auto_accept_re_pairing */);

  SMPairingInfo phone = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0xe2, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0xe2, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0xe2, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  bt_persistent_storage_store_ble_pairing(&phone, true /* is_gateway */, NULL,
                                          false /* requires_address_pinning */,
                                          false /* auto_accept_re_pairing */);

  // This is the passing state on the watch after a reboot: one of each, nothing deleted.
  bt_persistent_storage_get_ble_bonding_counts(&gateway, &non_gateway, &deleted);
  cl_assert_equal_i(gateway, 1);
  cl_assert_equal_i(non_gateway, 1);
  cl_assert_equal_i(deleted, 0);

  // Deleting the pump must be counted, so a failure reads as "pruned" not "never stored".
  bt_persistent_storage_delete_ble_pairing_by_id(pump_id);
  bt_persistent_storage_get_ble_bonding_counts(&gateway, &non_gateway, &deleted);
  cl_assert_equal_i(gateway, 1);
  cl_assert_equal_i(non_gateway, 0);
  cl_assert_equal_i(deleted, 1);

  // All out-params are optional.
  bt_persistent_storage_get_ble_bonding_counts(NULL, NULL, NULL);
}
```

Note: the test relies on the harness resetting storage between tests, which
`test_bluetooth_persistent_storage__initialize` already does (it is why the first assertions expect
zeros). The delete counter must therefore be reset there too — handled in Step 3.

- [ ] **Step 2: Run the test to verify it fails**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: a **compile** failure — `implicit declaration of function
'bt_persistent_storage_get_ble_bonding_counts'`. That is the correct failure for a new API.

- [ ] **Step 3: Write the implementation**

First, declare the accessor in `include/pbl/services/bluetooth/bluetooth_persistent_storage.h`,
immediately after the `bt_persistent_storage_is_ble_ancs_bonding` declaration on line 74:

```c
//! Diagnostic: inventory of stored BLE bondings by gateway status, plus a count of non-gateway
//! bondings deleted since boot. Lets a non-gateway (e.g. MiniMed pump) bond that has gone missing
//! be attributed to a deletion rather than a failed store. Any out-param may be NULL.
void bt_persistent_storage_get_ble_bonding_counts(uint8_t *gateway_out, uint8_t *non_gateway_out,
                                                  uint8_t *non_gateway_deleted_out);
```

Then in `bluetooth_persistent_storage_normal.c`, add the counter as a file-static near the other
statics at the top of the file:

```c
//! Non-gateway BLE bondings deleted since boot. Diagnostic only: distinguishes a pump bond that was
//! pruned from one that was never stored, which are indistinguishable from the watch otherwise.
static uint8_t s_non_gateway_deletes;
```

Bump it inside `prv_delete_ble_pairing_by_id`, which already has the deleted record in hand:

```c
static bool prv_delete_ble_pairing_by_id(BTBondingID bonding) {
  BtPersistBondingData deleted_data;
  if (!prv_delete_pairing_with_type_by_id(bonding, BtPersistBondingTypeBLE, &deleted_data)) {
    return false;
  }

  if (!deleted_data.ble_data.is_gateway && s_non_gateway_deletes < UINT8_MAX) {
    s_non_gateway_deletes++;
  }

  status_t rv;
  rv = prv_delete_all_cccd_for_addr(&deleted_data.ble_data.pairing_info.identity);
  PBL_ASSERTN(rv == S_SUCCESS);

  prv_remove_ble_bonding_from_bt_driver(&deleted_data);

  prv_call_ble_bonding_change_handlers(bonding, BtPersistBondingOpWillDelete);
  return true;
}
```

Add the counting iterator and the accessor next to the other iterators, for example just after
`bt_persistent_storage_is_ble_ancs_bonding`:

```c
typedef struct {
  uint8_t gateway;
  uint8_t non_gateway;
} BleBondingCountsItrData;

static bool prv_count_ble_bondings_itr(SettingsFile *file, SettingsRecordInfo *info,
                                       void *context) {
  if (info->val_len == 0 || info->key_len != sizeof(BTBondingID)) {
    return true;
  }

  BtPersistBondingData stored_data;
  info->get_val(file, (uint8_t *)&stored_data, MIN((unsigned)info->val_len, sizeof(stored_data)));
  if (stored_data.type != BtPersistBondingTypeBLE) {
    return true;
  }

  BleBondingCountsItrData *itr_data = context;
  if (stored_data.ble_data.is_gateway) {
    itr_data->gateway++;
  } else {
    itr_data->non_gateway++;
  }
  return true;
}

void bt_persistent_storage_get_ble_bonding_counts(uint8_t *gateway_out, uint8_t *non_gateway_out,
                                                  uint8_t *non_gateway_deleted_out) {
  BleBondingCountsItrData itr_data = { 0 };
  prv_file_each(prv_count_ble_bondings_itr, &itr_data);

  if (gateway_out) {
    *gateway_out = itr_data.gateway;
  }
  if (non_gateway_out) {
    *non_gateway_out = itr_data.non_gateway;
  }
  if (non_gateway_deleted_out) {
    *non_gateway_deleted_out = s_non_gateway_deletes;
  }
}
```

Finally, reset the counter at the **top** of `bt_persistent_storage_init`
(`bluetooth_persistent_storage_normal.c:1482`), before `prv_load_data_from_prf()`:

```c
void bt_persistent_storage_init(void) {
  // Note: this gets called well before the BT stack is initialized, make sure there is no code
  // that tries to use the BT stack in this path.
  s_non_gateway_deletes = 0;
  s_db_mutex = mutex_create();
```

Placement matters, and not only for the tests. `bt_persistent_storage_init` runs
`prv_prune_stale_ble_bondings()` a few lines later (`:1491`), so resetting *first* means a
boot-time prune of the pump bond **is** counted. That is the single most valuable reading the
diagnostic gives: after a reboot, `pmp0 del1` says the boot prune ate the bond, while `pmp0 del0`
says it was never stored. Resetting after the prune would erase exactly the evidence we want.

The test harness calls `bt_persistent_storage_init()` in
`test_bluetooth_persistent_storage__initialize`, which runs before every test, so this also gives
the test the zeros it expects on entry.

- [ ] **Step 4: Run the test to verify it passes**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test -M '.*bluetooth_persistent_storage.*'"
```

Expected: `tests that pass 2/2`, with all five new tests passing.

If the first three assertions fail with non-zero counts, the harness is not resetting between
tests — check that the `s_non_gateway_deletes = 0;` line landed in `bt_persistent_storage_init` and
that the test file's `initialize` function calls it.

- [ ] **Step 5: Render the inventory in the app**

In `src/fw/apps/system/minimed_sake_app.c`, replace `prv_refresh` with:

```c
static void prv_refresh(MinimedSakeAppData *data) {
  const char *mode = (minimed_sake_get_mode() == MinimedSakeModeSpike)
                         ? (minimed_sake_pump_paired() ? "MODE: SPIKE (FE81)" : "MODE: SPIKE (FE82)")
                         : "MODE: NORMAL";

  // Bond inventory: gw = phone bonds, pmp = pump (non-gateway) bonds, del = non-gateway bonds
  // deleted since boot. "FE81" above with pmp0 is the FE81/FE82 mismatch; pmp0 with del1 means
  // something pruned the pump bond; pmp0 with del0 means it was never stored.
  uint8_t gateway = 0, non_gateway = 0, deleted = 0;
  bt_persistent_storage_get_ble_bonding_counts(&gateway, &non_gateway, &deleted);

  snprintf(data->buf, sizeof(data->buf), "%s\nbond gw%u pmp%u del%u\n%s", mode, gateway,
           non_gateway, deleted, minimed_sake_get_log());
  text_layer_set_text(&data->text, data->buf);
}
```

Add the include if it is not already present — check with
`grep -n 'bluetooth_persistent_storage' src/fw/apps/system/minimed_sake_app.c`, and if there is no
match add `#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"` alongside the other
includes.

- [ ] **Step 6: Build the firmware and run the full test suite**

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH
    ./waf build" 2>&1 | tail -10
```

Expected: `'build' finished successfully`.

Then the whole suite, not just the filtered test, to catch collateral damage:

```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
  pebbleos-build:local bash -lc "
    git config --global --add safe.directory /pebbleos
    ./waf test" 2>&1 | tail -20
```

Expected: `'test' finished successfully` with no failures reported. Tests already on the
`BROKEN_TESTS` quarantine list (`test_gap_le_connect.c` and four graphics tests) are skipped by the
build and are not your concern.

- [ ] **Step 7: Commit**

```bash
git add src/fw/services/bluetooth/bluetooth_persistent_storage_normal.c \
        include/pbl/services/bluetooth/bluetooth_persistent_storage.h \
        src/fw/apps/system/minimed_sake_app.c \
        tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c
git commit -s -m "bluetooth: show the BLE bond inventory in the SAKE Spike app

If the bond-coexistence fix does not work, the pump link is exactly
what is broken -- so the phone log channel is unavailable by
definition, and the failure has to be readable on the watch itself.
Boot-time logging is not an option: the pruning runs in
services_common_init long before the BT stack, minimed_sake_log needs
kernel_malloc and the launcher task, and the 8-line ring scrolls away.

So report live state instead of history. The app's existing 400 ms
refresh now shows \"bond gw<N> pmp<N> del<N>\": stored gateway bonds,
stored non-gateway (pump) bonds, and non-gateway bonds deleted since
boot. That last counter is what separates \"the bond was pruned\" from
\"the bond was never stored\", which are otherwise indistinguishable.
FE81 in the mode line with pmp0 names the FE81/FE82 mismatch outright.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
uvx --from gitlint-core gitlint --commits HEAD~1..HEAD
```

---

### Task 5: Build the flashable bundle and update the docs

**Files:**
- Modify: `PROGRESS.md` (version log entry, remaining-work item 8 status)
- Modify: `TESTING.md` (the flash/pairing cost, which this change invalidates)
- Modify: `PebbleOS/.claude/CLAUDE.md` (the build/test snags found while writing this plan)

**Interfaces:**
- Consumes: all previous tasks.
- Produces: `build/sake-spike-v36-bond-coexistence.pbz`.

- [ ] **Step 1: Record the build/test snags in the project CLAUDE.md**

Append to the "Firmware development" section of `PebbleOS/.claude/CLAUDE.md`:

```markdown
- **Everything builds in Docker** (`pebbleos-build:local`); there is no local Python toolchain, so
  a bare `./waf` fails on a missing `intelhex`. `./spike-build.sh` wraps the firmware build.
- **Never run `./waf configure` without `--board`** — it wipes `build/c4che` and de-configures the
  firmware build. Restore with
  `./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y` (inside Docker).
- **Unit tests:** `./waf test` inside Docker. It uses the `test` waf variant (`build/test/`), so it
  does **not** clobber the firmware build and works with the asterix configure in place. The `-M`
  filter is an anchored `re.match` against the test source path, so `-M name` matches nothing and
  the run "succeeds" having built no tests — always write `-M '.*name.*'`. First run ~2 min, then
  ~1 s.
```

- [ ] **Step 2: Build the bundle**

Put the watch in NORMAL mode first (SAKE Spike app, SELECT until `MODE: NORMAL`) so the phone link
is up for the push. Then:

```bash
./spike-build.sh bond-coexistence
```

Expected: `>> build/sake-spike-v36-bond-coexistence.pbz` and a push confirmation. If the push is
skipped because adb sees no device, that is fine — sideload from wherever the file is.

- [ ] **Step 3: Update PROGRESS.md**

Insert this at the top of the version log, immediately above the v35 entry:

```markdown
- v36 (2026-07-26, **AWAITING HW**): **phone + pump bond coexistence — no more re-pairing.** The
  per-flash pump re-pair was never about the single connection slot; it was bond pruning. Three
  edits, all no-ops in stock where every BLE bond is a gateway: (1)
  `prv_collect_other_ble_bondings_itr` skips non-gateway bonds, which alone closes the phone-re-pair
  prune *and* both boot paths (the SPRF replay re-stores the phone as a gateway and lands in the
  same collector, and `prv_prune_stale_ble_bondings` deletes through it too); (2)
  `bt_persistent_storage_delete_ble_pairing_by_id` only erases the shared-PRF slot for gateway
  bonds, so forgetting the pump no longer wipes the phone's PRF record; (3) Settings → Bluetooth
  skips non-gateway bonds when building the phone list — **required**, not cosmetic, because
  pairability is gated on that list being empty, so a now-permanent pump bond would otherwise lock
  out phone pairing forever. Plus a live `bond gw<N> pmp<N> del<N>` readout in the SAKE Spike app:
  the `del` counter is reset at the top of `bt_persistent_storage_init` (before the boot prune), so
  after a reboot `pmp0 del1` means the prune ate the bond and `pmp0 del0` means it was never
  stored — and `FE81` in the mode line with `pmp0` names the FE81/FE82 mismatch outright. Host
  tests cover the storage logic; the settings filter is HW-only (no settings-app test harness
  exists). Completes v33, which fixed only the mirror image. Fallback: reflash v35.
```

Then update remaining-work item 8: the collector, SPRF-erase and Settings-pairability holes it
describes are now closed, so it should say what (if anything) Stage 2 still needs from that area
rather than continuing to describe the fixed state as pending.

Leave the entry marked **AWAITING HW** until the Validation checks have actually been run. Do not
write "verified" before it is — this project's history is full of entries that had to be walked back.

- [ ] **Step 4: Update TESTING.md**

The "What the flash costs you in pairings" section is now wrong. It currently states the pump bond
never survives a flash and prescribes a mandatory 5-step order built around that. Replace its cost
statement with:

```markdown
**On v36 and later** (bond coexistence — verify with the checks below before trusting it): a flash
costs **no pairings at all**. The phone bond survives as it always did, and the pump bond now
survives the reboot too, so the loop is: NORMAL → sideload → reboot → SELECT into SPIKE → pump
reconnects on its own (`paired (persisted): FE81` → `HANDSHAKE OK!`). Confirm with the bond
inventory line in the SAKE Spike app: `bond gw1 pmp1 del0` before and after.

**On v35 and earlier**, the pump bond was evicted at every boot (the boot SPRF replay re-stores the
phone as a gateway, and a gateway write pruned every other BLE bond), so every flash cost exactly
one pump re-pair and the phone-first / pump-second order below was mandatory.
```

Keep the old ordering instructions, clearly marked as applying to v35 and earlier, until v36 passes
its hardware checks — if the checks fail you will need them again.

- [ ] **Step 5: Commit**

```bash
git add PROGRESS.md TESTING.md .claude/CLAUDE.md
git commit -s -m "docs: bond coexistence (v36) and the docker build/test snags

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
uvx --from gitlint-core gitlint --commits HEAD~1..HEAD
```

---

## Validation (hardware — Morten runs these)

The unit tests prove the storage logic; only hardware proves the tax is gone. Follow the standing
discipline from `TESTING.md`: the on-watch log is an 8-line ring whose stale lines do not count, and
one clean cycle proves nothing because the historical bugs were timing-dependent.

**Check 1 — reboot survival (the one that proves the point).** With the pump paired, power-cycle the
watch. Open the SAKE Spike app and read the inventory line first: `bond gw1 pmp1 del0` is the pass.
Then SELECT into SPIKE and expect `paired (persisted): FE81` → pump reconnect → `HANDSHAKE OK!`,
with **no** remove/re-add on the pump.

- `pmp0 del1` — something still prunes the pump bond. The fix did not hold; capture the line and
  stop.
- `pmp0 del0` — the bond was never stored. A different bug from pruning; look at `nimble_store.c`.
- `gw0` — the phone bond is gone. That is the v33 regression direction; reflash v35.

**Check 2 — phone re-pair survival.** Forget the watch on the phone, pair it again in NORMAL, and
confirm the inventory still reads `pmp1 del0` before toggling to SPIKE. The pump must reconnect
without a re-pair. This is the direction v33 did not fix.

**Check 3 — settings pairability.** With the pump bond present, open Settings → Bluetooth. The pump
must not appear as a row, the header must not count it as a paired phone, and pairing a new phone
must still be offered — no "Forget this device to pair a new device" when only the pump is bonded.

**Check 4 — end-to-end iteration cost.** Sideload, reboot, SELECT into SPIKE, and confirm BG and IOB
come back with zero pairing actions on either device. That is the whole point of Stage 1.

**Fallback if any check fails:** reflash `build/sake-spike-v35-scanrsp-and-name-revert.pbz`. It is
the current known-good build (pump pairs, BG + IOB correct; the watchface crash is pre-existing and
unrelated).

**Known-broken and not caused by this work:** the watchface crashes on launch under v34/v35, and the
v34 "device not found" pairing failure whose v35 fix bundled two changes. Both are documented in the
OPEN sections of `PROGRESS.md`. Do not read either as a Stage 1 regression.

## After Stage 1 is hardware-verified

Update the `PROGRESS.md` v36 entry from "awaiting HW" to verified, then start Stage 2 with its own
spec and plan. Stage 2's findings are already recorded in
`docs/superpowers/specs/2026-07-26-dual-connection-design.md` section 2 — in particular that the
pump-link swallow does not exist yet, must cover the encryption-change event, and that
advertising-XOR-connected is the real blocker rather than the two connection booleans.
