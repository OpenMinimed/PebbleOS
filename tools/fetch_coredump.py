#!/usr/bin/env python3
"""Fetch the watch's coredump over the phone connection and save it.

Same transport as dump_flash_logs.py: needs the USB tunnel (adb forward
tcp:9000 tcp:9000) and Developer Connection enabled. The coredump is the
only way to resolve an assert's LR into the exact failing assertion when
there is no serial/USB access.

Usage:
    adb forward tcp:9000 tcp:9000
    python3 tools/fetch_coredump.py -o /tmp/watch_coredump.core
    python3 tools/analyze_coredump.py build/pebbleos.elf /tmp/watch_coredump.core
"""

import argparse
import os
import sys

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
    data = getbytes.get_coredump(require_fresh=args.fresh)

    if not data:
        print("No coredump present.")
        return 1

    with open(args.output, "wb") as f:
        f.write(data)
    print("Wrote {} bytes to {}".format(len(data), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())