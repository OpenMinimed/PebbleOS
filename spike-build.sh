#!/usr/bin/env bash
# One-shot build/version/deploy for the MiniMed SAKE spike firmware.
#
#   ./spike-build.sh <desc>              build + share to the phone (default board: asterix)
#   ./spike-build.sh <desc> --pt2        build for obelix / Pebble Time 2 instead
#   ./spike-build.sh <desc> --no-push    skip the share (just build the versioned .pbz files)
#   ./spike-build.sh --configure <desc>  force a waf configure (after Kconfig/registry changes)
#
# Two boards, two recipes. They differ in more than the --board flag, hence the profile block
# below rather than one parametrised path:
#
# asterix (Pebble 2 Duo, nRF52840) -- the default, Morten's watch:
#   - Single bundle. asterix has one firmware slot, so there is no slot dance.
#   - Non-release build against the locally built image.
#
# obelix@pvt (Pebble Time 2, SiFli SF32LB52) -- palmarci's watch, --pt2:
# (see PROGRESS.md "PT2 port" for the full post-mortem)
#   - Release build (CONFIG_RELEASE=y). The very first black-screens were caused by this flag
#     being silently dropped, making non-release builds that hung the obelix display/boot path.
#   - Separate, correctly-linked slot0 AND slot1 bundles. The Pebble app resolves a sideload to
#     the slot NOT currently running (updateToSlot = 1 - runningSlot) and its safety check requires
#     firmware.slot == updateToSlot, so a single slot0-only pbz "does not parse" whenever the watch
#     is running slot0. Building each slot as its OWN single-bundle pbz gives a file the app always
#     accepts. Do NOT dual-slot repack: the earlier repack baked the SAME slot0-linked image into
#     both slots, and the mislinked slot1 copy black-screened.
#   - Release band (0x01) with a release-form version, so it boots over the current stock
#     (4.36.2). A plain annotated git tag like v4.36.9 gives the app a parseable versionTag.
#     Dev band (0x80) is NOT used: a dev-form git describe breaks the app's manifest parse.
set -euo pipefail
cd "$(dirname "$0")"

profile=asterix
do_configure=0
push=1
desc=""
for arg in "$@"; do
  case "$arg" in
    --configure)        do_configure=1 ;;
    --no-push)          push=0 ;;
    --pt2|--obelix)     profile=obelix ;;
    --asterix)          profile=asterix ;;
    -*)                 echo "unknown flag: $arg" >&2; exit 2 ;;
    *)                  desc="$arg" ;;
  esac
done
[ -n "$desc" ] || { echo "usage: $0 <desc> [--pt2] [--configure] [--no-push]" >&2; exit 2; }

if [ "$profile" = obelix ]; then
  IMAGE=ghcr.io/coredevices/pebbleos-docker:v6  # official CI image, not the local commit
  BOARD=obelix@pvt                              # PT2 / Pebble Time 2 (SiFli), production revision
  DOCKER_USER=()                                # the CI image needs root to pip install
  PIP_CMD='pip install -U pip >/dev/null 2>&1; pip install -r requirements.txt >/dev/null 2>&1;'
  CORE_CFG="-DCONFIG_RELEASE=y -DCONFIG_MINIMED_SAKE_SPIKE=y"
  SLOTS=(0 1)
  NEED_TAG=1
  VERIFY_BAND=1
else
  IMAGE=pebbleos-build:local
  BOARD=asterix                                 # Pebble 2 Duo (nRF52840)
  DOCKER_USER=(-u "$(id -u):$(id -g)")
  PIP_CMD=''
  CORE_CFG="-DCONFIG_MINIMED_SAKE_SPIKE=y"
  SLOTS=()                                      # one slot: no -DCONFIG_FIRMWARE_SLOT
  NEED_TAG=0
  VERIFY_BAND=0
fi
BOARD_NORM=${BOARD//@/_}                        # obelix_pvt (BOARD_NORMALIZED strips @revision)
SPIKE_TAG=${SPIKE_TAG:-v4.36.9}                 # release-form tag stamped into the bundle
echo ">> board $BOARD (image $IMAGE)"

if [ "$NEED_TAG" = 1 ]; then
  # Ensure a release-form annotated tag exists on HEAD so `git describe` in the build resolves to
  # something the Pebble app parses (vX.Y.Z / -beta / -rc) AND that encodes as release band.
  # If SPIKE_TAG exists on an older commit, move it to HEAD (the bundle carries the HEAD build).
  git tag -f -a "$SPIKE_TAG" -m "spike pt2 build" HEAD >/dev/null 2>&1
  git describe --dirty
fi

# Fast path: keep the existing build/c4che configure (incremental) unless one is missing
# or the board/config changed. Re-configure only on --configure or first run.
if [ -d build/c4che ] && ! grep -q "BOARD = '${BOARD%@*}'" build/c4che/_cache.py 2>/dev/null; then
  do_configure=1
fi
[ -d build/c4che ] || do_configure=1

# Next version = 1 + (max N across build/sake-spike-vN-*.pbz) (numeric, not lexical).
next_ver=$(( $(ls build/sake-spike-v*.pbz 2>/dev/null \
  | sed -n 's#.*/sake-spike-v\([0-9]\{1,\}\)-.*#\1#p' | sort -n | tail -1 | grep -E '^[0-9]+$' || echo 0) + 1 ))

# Build one image. With an argument it is a slot number (obelix); without, the board's single slot.
build_slot() {
  local slot=${1:-}
  local cfg=""
  # Configure if forced, or if the existing cache lacks the spike config, targets a different slot,
  # or (obelix) is not a release build. Guards against stale caches from a plain configure.
  if [ "$do_configure" = 1 ] || ! grep -q "MINIMED_SAKE_SPIKE" build/c4che/_cache.py 2>/dev/null; then
    cfg="true"
  fi
  if [ -n "$slot" ] && ! grep -q "FIRMWARE_SLOT = $slot" build/c4che/_cache.py 2>/dev/null; then
    cfg="true"
  fi
  if [ "$VERIFY_BAND" = 1 ] && ! grep -qE "CONFIG_RELEASE\s*=\s*(1|True)" build/c4che/_cache.py 2>/dev/null; then
    cfg="true"
  fi
  echo ">> building ${slot:+slot$slot }(v$next_ver-$desc)${cfg:+ [configure]}..."
  docker run --rm "${DOCKER_USER[@]}" -e HOME=/tmp \
    -v "$PWD":/pebbleos -w /pebbleos "$IMAGE" bash -lc "
      git config --global --add safe.directory /pebbleos
      $PIP_CMD
      export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:\$PATH
      ${cfg:+./waf configure --board $BOARD ${slot:+-DCONFIG_FIRMWARE_SLOT=$slot} $CORE_CFG && }./waf build && ./waf bundle"
}

# Only obelix boots via the Pebble app's release-band check; asterix takes whatever we build.
verify_bundle() {
  local out=$1
  [ "$VERIFY_BAND" = 1 ] || return 0
  local version_tag band_hex maj min pat
  version_tag=$(unzip -p "$out" manifest.json | python3 -c "import json,sys; print(json.load(sys.stdin)['firmware']['versionTag'])")
  read band_hex maj min pat <<<"$(python3 -c "
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

outs=()
if [ ${#SLOTS[@]} -eq 0 ]; then
  build_slot
  fresh=$(ls -t build/normal_${BOARD_NORM}_*.pbz | head -1)
  out="build/sake-spike-v${next_ver}-${desc}.pbz"
  cp "$fresh" "$out"
  verify_bundle "$out"
  echo ">> $out"
  outs+=("$out")
else
  for slot in "${SLOTS[@]}"; do
    build_slot "$slot"
    fresh=$(ls -t build/normal_${BOARD_NORM}_*slot${slot}.pbz | head -1)
    out="build/sake-spike-v${next_ver}-${desc}_slot${slot}.pbz"
    cp "$fresh" "$out"
    verify_bundle "$out"
    outs+=("$out")
  done
fi

# Keep this build's loghash dictionary next to the .pbz. PBL_LOG lines are stored hashed and the
# hashes change between builds, so without the matching dict tools/dump_flash_logs.py cannot read
# back a log written by an older firmware. (SAME dict for both slots.)
if [ -f build/pebbleos_loghash_dict.json ]; then
  cp build/pebbleos_loghash_dict.json "build/sake-spike-v${next_ver}-${desc}.loghash.json"
fi

if [ "$push" = 1 ]; then
  if command -v adb >/dev/null 2>&1 && adb get-state 2>/dev/null | grep -q device; then
    # adb first: over USB or `adb connect IP:port` (also the tunnel to the phone's 9000 port for
    # pebble logs / dump_flash_logs). Lands in the phone's Downloads so the Pebble app's file
    # picker can find it.
    for out in "${outs[@]}"; do
      adb push "$out" /sdcard/Download/ >/dev/null 2>&1 && \
        echo ">> pushed to phone: $(basename "$out")"
    done
    if [ ${#outs[@]} -gt 1 ]; then
      echo ">> Flash the one whose slot the app wants (watch runs <n> -> app wants 1-<n>)."
    fi
  else
    device=$(kdeconnect-cli -a --id-only 2>/dev/null | head -1)
    if [ -n "$device" ]; then
      for out in "${outs[@]}"; do
        kdeconnect-cli -d "$device" --share "$out" >/dev/null && \
          echo ">> shared to phone: $(basename "$out")"
      done
      if [ ${#outs[@]} -gt 1 ]; then
        echo ">> Flash the one whose slot the app wants (watch runs <n> -> app wants 1-<n>)."
      fi
    else
      echo ">> skip share: no adb device and no reachable kdeconnect device (use --no-push to silence)"
    fi
  fi
fi
