#!/usr/bin/env bash
# One-shot build/version/deploy for the MiniMed SAKE spike firmware.
#
#   ./spike-build.sh <desc>          build + bundle, copy to build/sake-spike-vN-<desc>.pbz,
#                                    and push it to the phone's Download folder over adb.
#   ./spike-build.sh <desc> --no-push    skip the adb push (just build the versioned .pbz)
#   ./spike-build.sh --configure <desc>  re-run waf configure first (after Kconfig/registry changes)
#
# Why this exists: the raw Docker one-liner is long, `./waf bundle` always emits the same
# git-describe name (easy to grab a stale one), the versioned copy was manual, and getting the
# file to the phone was a separate step. This does all four. See PROGRESS.md.
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=pebbleos-build:local
PHONE_DIR=/sdcard/Download
do_configure=0
push=1
desc=""
for arg in "$@"; do
  case "$arg" in
    --configure) do_configure=1 ;;
    --no-push)   push=0 ;;
    -*)          echo "unknown flag: $arg" >&2; exit 2 ;;
    *)           desc="$arg" ;;
  esac
done
[ -n "$desc" ] || { echo "usage: $0 <desc> [--configure] [--no-push]" >&2; exit 2; }
[ -d build/c4che ] || do_configure=1   # never configured yet -> must configure

# Next version = 1 + highest N across existing build/sake-spike-vN-*.pbz (numeric, not lexical).
next_ver=$(( $(ls build/sake-spike-v*.pbz 2>/dev/null \
  | sed -n 's#.*/sake-spike-v\([0-9]\{1,\}\)-.*#\1#p' | sort -n | tail -1 | grep -E '^[0-9]+$' || echo 0) + 1 ))

cfg='true'
[ "$do_configure" = 1 ] && cfg='./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y'
echo ">> building (v$next_ver-$desc)${do_configure:+ [configure]}..."
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD":/pebbleos -w /pebbleos "$IMAGE" bash -lc "
    git config --global --add safe.directory /pebbleos
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH
    $cfg && ./waf build && ./waf bundle"

# The freshly written bundle is the newest normal_asterix_*.pbz.
fresh=$(ls -t build/normal_asterix_*.pbz | head -1)
out="build/sake-spike-v${next_ver}-${desc}.pbz"
cp "$fresh" "$out"
echo ">> $out"

if [ "$push" = 1 ]; then
  n=$(adb devices | grep -cw device || true)
  if [ "$n" = 1 ]; then
    adb push "$out" "$PHONE_DIR/" >/dev/null && echo ">> pushed to phone $PHONE_DIR/$(basename "$out")"
  else
    echo ">> skip push: expected 1 adb device, found $n (use --no-push to silence)"
  fi
fi
