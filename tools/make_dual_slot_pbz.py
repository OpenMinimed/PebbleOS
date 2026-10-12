#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0
"""Repackage a single-slot firmware pbz as the dual-slot layout the Pebble app
(Core Devices / mobile-app) expects for sideloading onto a dual-slot watch.

The app looks up firmware by the slot *not* currently running
(DiskUtil.requirePbzManifests + PbzFirmware.findManifestFor), so a normal build
bundled with only a root manifest (slot 0) fails with
"IllegalStateException: No manifest for slot <n>". The reference layout that the
app's own tests ship (libpebble3/src/jvmTest/resources/normal_obelix_test.pbz)
puts per-slot copies under slot0/ and slot1/ instead:

  slot0/manifest.json  slot0/tintin_fw.bin  slot0/system_resources.pbpack
  slot1/manifest.json  slot1/tintin_fw.bin  slot1/system_resources.pbpack

Both slots carry the same binary here: whichever slot the app targets, it gets
the same firmware. Slot-indexing the same image twice has no downside; the pblboot
priority header decides which boots.

Usage:
  make_dual_slot_pbz.py <single-slot.pbz> <dual-slot.pbz>
"""

import json
import os
import sys
import time
import zipfile

try:
    import stm32_crc
except ImportError:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "."))
    import stm32_crc

FW_NAME = "tintin_fw.bin"
EXTRA_COPIED = ("LICENSE", "layouts.json.auto", "js_tooling.js")


def _stm32_crc(data):
    return stm32_crc.crc32(data) & 0xFFFFFFFF


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    in_path, out_path = sys.argv[1:3]

    with zipfile.ZipFile(in_path) as zin:
        manifest = json.loads(zin.read("manifest.json"))
        firmware_entry = {
            "name": FW_NAME,
            "type": "normal",
            "timestamp": manifest["firmware"]["timestamp"],
            "commit": manifest["firmware"]["commit"],
            "hwrev": manifest["firmware"]["hwrev"],
            "versionTag": manifest["firmware"]["versionTag"],
        }
        fw_bytes = zin.read(manifest["firmware"]["name"])
        resources_entry = None
        res_bytes = None
        if manifest.get("resources"):
            res_bytes = zin.read(manifest["resources"]["name"])
            resources_entry = {
                "name": "system_resources.pbpack",
                "timestamp": manifest["resources"]["timestamp"],
                "size": len(res_bytes),
                "crc": _stm32_crc(res_bytes),
            }
        extras = {}
        for name in EXTRA_COPIED:
            if name in zin.namelist():
                extras[name] = zin.read(name)

    firmware_entry["size"] = len(fw_bytes)
    firmware_entry["crc"] = _stm32_crc(fw_bytes)

    generated_at = int(time.time())
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zout:
        for slot in (0, 1):
            prefix = f"slot{slot}/"
            slot_manifest = {
                "manifestVersion": 2,
                "generatedAt": generated_at,
                "generatedBy": "",
                "debug": {},
                "firmware": dict(firmware_entry, slot=slot),
                "type": "firmware",
            }
            zout.writestr(prefix + "manifest.json", json.dumps(slot_manifest))
            zout.writestr(prefix + FW_NAME, fw_bytes)
            if resources_entry is not None and res_bytes is not None:
                slot_manifest["resources"] = resources_entry
                zout.writestr(prefix + resources_entry["name"], res_bytes)
            for name, data in extras.items():
                zout.writestr(prefix + name, data)

    print(f"wrote dual-slot pbz: {out_path}")
    print(f"  firmware {FW_NAME} {firmware_entry['size']} bytes crc={firmware_entry['crc']}")


if __name__ == "__main__":
    main()