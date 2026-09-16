#!/usr/bin/env python3
"""Fetch the watch's coredump over the phone connection and save it.

Same transport as dump_flash_logs.py: needs the USB tunnel (adb forward
tcp:9000 tcp:9000) and Developer Connection enabled. The coredump is the
only way to resolve an assert's LR into the exact failing assertion when
there is no serial/USB access.

A plain (non --fresh) fetch that reads every byte of the coredump already
acks it: src/fw/services/get_bytes/get_bytes.c calls gb_storage_cleanup(...,
successful=true) purely because the transfer completed, with no separate
client-side ack message. That cleanup calls core_dump_mark_read(), which
clears the "unread" flag core_dump_reset() checks. So a fully successful
fetch clears the 24h freshness-protection window immediately: the very next
crash gets its own fresh core dump instead of skipping the write for up to
CORE_DUMP_MIN_AGE_SECONDS. The only way this doesn't happen is a transfer
that dies partway (BLE drop) — the firmware never reaches "successful" and
the flag stays set. --retries below guards against that, and --verify
confirms the ack actually landed.

Usage:
    adb forward tcp:9000 tcp:9000
    python3 tools/fetch_coredump.py -o /tmp/watch_coredump.core
    python3 tools/analyze_coredump.py build/pebbleos.elf /tmp/watch_coredump.core
"""

import argparse
import os
import sys
import time

def _reexec_with_pebble_tool_python():
    import shutil

    if os.environ.get("_FETCH_COREDUMP_REEXEC"):
        return

    candidates = []
    pebble = shutil.which("pebble")
    if pebble:
        with open(pebble, "rb") as f:
            first = f.readline().decode("utf-8", "replace").strip()
        if first.startswith("#!"):
            candidates.append(first[2:].split()[0])
    candidates.append(os.path.expanduser("~/.local/share/uv/tools/pebble-tool/bin/python3"))

    for python in candidates:
        if python and python != sys.executable and os.access(python, os.X_OK):
            os.environ["_FETCH_COREDUMP_REEXEC"] = "1"
            os.execv(python, [python, os.path.abspath(__file__)] + sys.argv[1:])


try:
    from libpebble2.communication import PebbleConnection
    from libpebble2.communication.transports.websocket import WebsocketTransport
    from libpebble2.services.getbytes import GetBytesService
    from libpebble2.exceptions import GetBytesError
except ImportError:
    _reexec_with_pebble_tool_python()
    raise


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--phone", default="127.0.0.1",
                    help="phone address (default 127.0.0.1, i.e. the adb USB tunnel)")
    ap.add_argument("-o", "--output", default="/tmp/watch_coredump.core",
                    help="output file for the coredump")
    ap.add_argument("--fresh", action="store_true",
                    help="only fetch a coredump that has not been read yet")
    ap.add_argument("--retries", type=int, default=3,
                    help="retry a transfer that dies partway (leaves the coredump unread "
                         "on the watch) this many times before giving up (default 3)")
    ap.add_argument("--no-verify", dest="verify", action="store_false",
                    help="skip the post-fetch ack check (on by default): confirming the watch "
                         "actually cleared its unread flag via a cheap --fresh probe; no extra "
                         "data transfer unless the ack unexpectedly did not take")
    ap.set_defaults(verify=True)
    args = ap.parse_args()

    connection = PebbleConnection(WebsocketTransport("ws://{}:9000/".format(args.phone)))
    try:
        connection.connect()
        connection.run_async()
    except Exception as e:
        print("Could not reach the phone at {}:9000 ({})".format(args.phone, e))
        print("Run `adb forward tcp:9000 tcp:9000`, and check the Pebble app has Developer")
        print("Connection enabled and the watch connected (NORMAL mode, not SPIKE).")
        return 1

    getbytes = GetBytesService(connection)

    data = None
    last_err = None
    attempts = max(1, args.retries)
    for attempt in range(1, attempts + 1):
        try:
            data = getbytes.get_coredump(require_fresh=args.fresh)
            break
        except GetBytesError as e:
            last_err = e
            # DoesNotExist (3) means there's genuinely nothing to fetch (fresh with none
            # unread, or no coredump at all) -- retrying won't help, stop immediately.
            if e.code == 3:
                break
            print("Fetch attempt {}/{} failed ({}), retrying...".format(
                attempt, attempts, e))
            time.sleep(1)

    if data is None:
        if last_err is not None:
            print("No coredump present ({}).".format(last_err))
        else:
            print("No coredump present.")
        return 1

    with open(args.output, "wb") as f:
        f.write(data)
    print("Wrote {} bytes to {}".format(len(data), args.output))
    print("Transfer completed in full, so the watch already cleared its unread flag: "
          "the next crash gets its own fresh core dump immediately, no 24h wait.")

    if args.verify and not args.fresh:
        try:
            getbytes.get_coredump(require_fresh=True)
            print("WARNING: watch still reports an unread coredump after a full fetch -- "
                  "the ack did not take. Investigate before assuming the 24h window cleared.")
        except GetBytesError as e:
            if e.code == 3:
                print("Verified: watch confirms no unread coredump remains.")
            else:
                print("Verification probe failed ({}); ack status unconfirmed.".format(e))

    return 0


if __name__ == "__main__":
    sys.exit(main())