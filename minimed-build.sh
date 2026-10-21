#!/usr/bin/env bash
# One-shot build/version/deploy for the MiniMed firmware.
set -euo pipefail
cd "$(dirname "$0")"

profile=""
push=0
allow_dirty=0
desc=""
extra_cfg=""
usage() {
  cat <<'EOF'
usage: minimed-build.sh [-h] -t TARGET [--push] [--allow-dirty] [-DCONFIG_...=y|n ...] [desc]

Build MiniMed Pebble firmware, archive ELF/dict, and optionally push to phone.

positional arguments:
  desc                  Optional description suffix for build artifacts

options:
  -h, --help            Show this help message and exit
  -t, --target TARGET   Target board:
                          asterix  Pebble 2 Duo (nRF52840)
                          obelix   Pebble Time 2 (SiFli SF32LB52)
                          getafix  Pebble Round 2 (SiFli SF32LB52)
  --push                Push built .pbz to phone via adb or kdeconnect
  --allow-dirty         Allow building with uncommitted changes
  -DCONFIG_...=y|n      Extra Kconfig settings, e.g. -DCONFIG_MINIMED_PREDICTOR=n. The build's
                        .config is saved next to the .pbz, so the build stays traceable.
EOF
}
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)          usage; exit 0 ;;
    -t|--target)        profile="$2"; shift 2 ;;
    --push)             push=1; shift ;;
    --allow-dirty)      allow_dirty=1; shift ;;
    -DCONFIG_*)         extra_cfg+=" $1"; shift ;;
    -*)                 echo "unknown flag: $1" >&2; exit 2 ;;
    *)                  desc="$1"; shift ;;
  esac
done
[ -n "$profile" ] || { echo "ERROR: --target is required" >&2; usage >&2; exit 2; }

case "$profile" in
  asterix|obelix|getafix) ;;
  *) echo "ERROR: unknown target '$profile' (expected asterix, obelix, or getafix)" >&2; exit 2 ;;
esac

DESC_SUFFIX=${desc:+-${desc}}

# Every build must trace back to a commit: refuse a dirty tree unless explicitly bypassed.
# (Untracked files do not count: git describe --dirty ignores them too.)
if [ "$allow_dirty" != 1 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "ERROR: working tree has uncommitted changes; commit first (or pass --allow-dirty)." >&2
  exit 1
fi

IMAGE=pebbleos-minimed:v10
if [ "$profile" = obelix ]; then
  BOARD=obelix@pvt                              # PT2 / Pebble Time 2 (SiFli), production revision
  CORE_CFG="-DCONFIG_RELEASE=y -DCONFIG_MINIMED_SAKE=y"
  SLOTS=(0 1)
  NEED_TAG=1
  VERIFY_BAND=1
elif [ "$profile" = getafix ]; then
  BOARD=getafix@dvt2                            # Pebble Round 2 (SiFli), DVT2 revision
  CORE_CFG="-DCONFIG_RELEASE=y -DCONFIG_MINIMED_SAKE=y"
  SLOTS=(0 1)
  NEED_TAG=1
  VERIFY_BAND=1
else
  BOARD=asterix                                 # Pebble 2 Duo (nRF52840)
  # The predictor costs ~3.7 KB of kernel heap, enough to fail HEAP_FLOOR. Pass
  # -DCONFIG_MINIMED_PREDICTOR=y to build it anyway.
  CORE_CFG="-DCONFIG_MINIMED_SAKE=y -DCONFIG_SERVICE_VOICE_ENDPOINT=n -DCONFIG_MINIMED_PREDICTOR=n"
  SLOTS=()                                      # one slot: no -DCONFIG_FIRMWARE_SLOT
  NEED_TAG=0
  VERIFY_BAND=0
  # The kernel heap is the RAM left after all statics. Idle use is ~15 KB, and an incoming call
  # needs ~17 KB more (one 13.6 KB icon block); below that the watch reboots on every call.
  HEAP_FLOOR=33000
fi
CORE_CFG+=$extra_cfg
BOARD_NORM=${BOARD//@/_}                        # obelix_pvt, getafix_dvt2 (the bundle name has no @revision)
RELEASE_TAG=${RELEASE_TAG:-v4.41.9}              # release-form tag stamped into the bundle

# BOARD_SHORT keeps the build/ listing unambiguous when the same desc is built for both watches.
case "$profile" in
  obelix) BOARD_SHORT=pt2 ;;
  getafix) BOARD_SHORT=getafix ;;
  *) BOARD_SHORT=asterix ;;
esac

# Identity BEFORE the release-tag dance below: once HEAD carries that tag, git describe collapses
# to just the tag name (no hash, no commit count), which is the whole identity.
DESCRIBE=$(git describe --dirty --always)
echo ">> board $BOARD (image $IMAGE)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">> building $IMAGE (once)..."
  docker build -t "$IMAGE" -f tools/minimed/Dockerfile .
fi

if [ "$NEED_TAG" = 1 ]; then
  # Ensure a release-form annotated tag exists on HEAD so `git describe` in the build resolves to
  # something the Pebble app parses (vX.Y.Z / -beta / -rc) AND that encodes as release band.
  # If RELEASE_TAG exists on an older commit, move it to HEAD (the bundle carries the HEAD build).
  git tag -f -a "$RELEASE_TAG" -m "MiniMed $profile build" HEAD >/dev/null 2>&1
  git describe --dirty
fi

# Build one image. With an argument it is a slot number (obelix); without, the board's single slot.
# Always reconfigures: it is quick, and it keeps the cache from drifting away from CORE_CFG. CMake
# keeps every -DCONFIG_* it was ever given, so -U drops them first; a flag removed from CORE_CFG
# then really goes back to its default. A failed build exits, so no older .pbz is picked up.
build_slot() {
  local slot=${1:-}
  local dir="build-${BOARD_SHORT}${slot:+-slot$slot}"
  echo ">> building ${slot:+slot$slot }(${DESCRIBE}${DESC_SUFFIX}) in $dir..." >&2
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp/home \
    -v "$PWD":/pebbleos -w /pebbleos "$IMAGE" bash -lc "
      mkdir -p \$HOME
      git config --global --add safe.directory /pebbleos
      pbl configure -b $dir --board $BOARD -U 'CONFIG_*' ${slot:+-DCONFIG_FIRMWARE_SLOT=$slot} \
        $CORE_CFG && pbl build -b $dir && pbl bundle -b $dir" >&2 || exit 1
  echo "$dir"
}

# Only obelix/getafix boot via the Pebble app's release-band check; asterix takes whatever.
verify_bundle() {
  local out=$1
  [ "$VERIFY_BAND" = 1 ] || return 0
  if [ "$allow_dirty" = 1 ]; then
    return 0
  fi
  local version_tag band_hex maj min pat
  version_tag=$(unzip -p "$out" manifest.json | python3 -c "import json,sys; print(json.load(sys.stdin)['firmware']['versionTag'])")
  read -r band_hex maj min pat <<<"$(python3 -c "
import zipfile
fw = zipfile.ZipFile('$out').read('pebbleos.bin')
prio = int.from_bytes(fw[8:16], 'little')
print(f'{(prio>>56)&0xff:02x} {(prio>>48)&0xff} {(prio>>40)&0xff} {(prio>>32)&0xff}')
")"
  echo ">> $out: versionTag=$version_tag band=$band_hex v$maj.$min.$pat"
  if [ "$band_hex" != "01" ]; then
    echo ">> ERROR: expected release band 0x01, got $band_hex."
    exit 1
  fi
  if [ "$maj" -lt 4 ] || { [ "$maj" -eq 4 ] && [ "$min" -lt 36 ]; }; then
    echo ">> ERROR: version v$maj.$min.$pat would NOT boot over stock v4.36.2"
    exit 1
  fi
}

# Archive the linked ELF right after each slot's build. The two slots link at different addresses
# and a later build replaces the ELF, so a later coredump cannot be resolved against it. Keep a
# per-build, per-slot copy with full debug info for analyze_coredump.py/addr2line.
archive_elf() {
  local dir=$1 slot=${2:-}
  local elf="build/elfs/minimed-${BOARD_SHORT}-${DESCRIBE}${DESC_SUFFIX}${slot:+_slot${slot}}.elf"
  mkdir -p build/elfs
  cp "$dir/pebbleos.elf" "$elf"
  echo ">> archived: $elf"
}

# Fail a build whose kernel heap is below HEAP_FLOOR. Run on the ELF after linking, so new statics
# anywhere in the firmware count.
check_heap() {
  local dir=$1
  [ -n "${HEAP_FLOOR:-}" ] || return 0
  local start end size
  start=$(nm "$dir/pebbleos.elf" | awk '$3 == "_heap_start" {print $1}')
  end=$(nm "$dir/pebbleos.elf" | awk '$3 == "_heap_end" {print $1}')
  size=$(( 0x$end - 0x$start ))
  echo ">> kernel heap: $size B (floor $HEAP_FLOOR B)"
  if [ "$size" -lt "$HEAP_FLOOR" ]; then
    echo ">> ERROR: kernel heap below the floor; free static RAM first (see .agents/PROGRESS.md)." >&2
    exit 1
  fi
}

mkdir -p build
outs=()
if [ ${#SLOTS[@]} -eq 0 ]; then
  dir=$(build_slot)
  archive_elf "$dir"
  check_heap "$dir"
  fresh=$(ls -t "$dir"/normal_"${BOARD_NORM}"_*.pbz | head -1)
  out="build/minimed-${BOARD_SHORT}-${DESCRIBE}${DESC_SUFFIX}.pbz"
  cp "$fresh" "$out"
  verify_bundle "$out"
  echo ">> $out"
  outs+=("$out")
else
  for slot in "${SLOTS[@]}"; do
    dir=$(build_slot "$slot")
    archive_elf "$dir" "$slot"
    fresh=$(ls -t "$dir"/normal_"${BOARD_NORM}"_*slot"${slot}".pbz | head -1)
    out="build/minimed-${BOARD_SHORT}-${DESCRIBE}${DESC_SUFFIX}_slot${slot}.pbz"
    cp "$fresh" "$out"
    verify_bundle "$out"
    outs+=("$out")
  done
fi

# Keep this build's loghash dictionary next to the .pbz. PBL_LOG lines are stored hashed and the
# hashes change between builds, so without the matching dict tools/dump_flash_logs.py cannot read
# back a log written by an older firmware. (SAME dict for both slots.)
if [ -f "$dir/pebbleos_loghash_dict.json" ]; then
  cp "$dir/pebbleos_loghash_dict.json" "build/minimed-${BOARD_SHORT}-${DESCRIBE}${DESC_SUFFIX}.loghash.json"
fi
# The Kconfig result, since -DCONFIG_* arguments are not in the commit.
cp "$dir/.config" "build/minimed-${BOARD_SHORT}-${DESCRIBE}${DESC_SUFFIX}.config"

# The phone's file picker shows only the first ~25 characters. Copies sent to it go in a folder
# per board, minimed-<board>/, and drop that prefix from the name: <describe>[-desc][_slotN].pbz.
PHONE_DIR="minimed-${BOARD_SHORT}"
phone_copy() {
  local name
  name=$(basename "$1")
  name=${name#"${PHONE_DIR}"-}
  mkdir -p "build/phone/$PHONE_DIR"
  cp "$1" "build/phone/$PHONE_DIR/$name"
  echo "build/phone/$PHONE_DIR/$name"
}

if [ "$push" = 1 ]; then
  for i in "${!outs[@]}"; do
    outs[i]=$(phone_copy "${outs[i]}")
  done
  if command -v adb >/dev/null 2>&1 && adb get-state 2>/dev/null | grep -q device; then
    # adb first: over USB or `adb connect IP:port` (also the tunnel to the phone's 9000 port for
    # pebble logs / dump_flash_logs). Lands in the phone's Downloads so the Pebble app's file
    # picker can find it.
    adb shell mkdir -p "/sdcard/Download/$PHONE_DIR" >/dev/null 2>&1
    for out in "${outs[@]}"; do
      adb push "$out" "/sdcard/Download/$PHONE_DIR/" >/dev/null 2>&1 && \
        echo ">> pushed to phone: Download/$PHONE_DIR/$(basename "$out")"
    done
    # Files pushed over adb are not in the media index, and the phone's file picker reads that, so
    # it would show the folder as empty. Needs Android 11 or later.
    adb shell content call --uri content://media --method scan_volume --arg external_primary \
      >/dev/null 2>&1 || echo ">> media rescan failed; the file picker may not show the build yet"
    if [ ${#outs[@]} -gt 1 ]; then
      echo ">> Flash the one whose slot the app wants (watch runs <n> -> app wants 1-<n>)."
    fi
  else
    device=""
    if command -v kdeconnect-cli >/dev/null 2>&1; then
      device=$(kdeconnect-cli -a --id-only 2>/dev/null | head -1 || true)
    fi
    if [ -n "$device" ]; then
      for out in "${outs[@]}"; do
        kdeconnect-cli -d "$device" --share "$out" >/dev/null && \
          echo ">> shared to phone: $(basename "$out")"
      done
      if [ ${#outs[@]} -gt 1 ]; then
        echo ">> Flash the one whose slot the app wants (watch runs <n> -> app wants 1-<n>)."
      fi
    else
      echo ">> skip share: no adb device and no reachable kdeconnect device"
    fi
  fi
fi
