#!/bin/bash
# SPDX-FileCopyrightText: 2026 Pal Marci
# SPDX-License-Identifier: Apache-2.0
#
# Boot the MiniMed firmware in QEMU and check that it survives: the MiniMed task starts, the
# stand-in pump link (subsys/bluetooth/minimed_fake_pump.c) comes up, and the watch neither
# asserts nor reboots. Not a pump simulation -- a crash test for the task integration.
#
#   tools/minimed/qemu_smoke_test.sh [seconds]     (default 200)
#
# Builds qemu_emery with CONFIG_MINIMED_SAKE into build-qemu/ (override with BUILD_DIR), apart
# from the build/ the real boards use. Uses the pebbleos-minimed:v10 image if it exists (see
# tools/minimed/Dockerfile), else the CI image plus the missing packages. Leaves the serial logs in
# $BUILD_DIR/qemu_uart{1,2,3}.log and a screenshot in $BUILD_DIR/qemu_shot.ppm.

set -e
cd "$(dirname "$0")/../.."
SECS=${1:-200}
BUILD_DIR=${BUILD_DIR:-build-qemu}

if docker image inspect pebbleos-minimed:v10 >/dev/null 2>&1; then
  IMAGE=pebbleos-minimed:v10
  SETUP=':'
else
  IMAGE=ghcr.io/coredevices/pebbleos-docker:v10
  SETUP='git config --global --add safe.directory /pebbleos
    (apt-get update -qq && apt-get install -y -qq libsdl2-2.0-0) >/dev/null 2>&1
    pip install -r requirements.txt >/dev/null 2>&1
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:/opt/pebbleos-sdk/qemu/bin:$PATH'
fi

mkdir -p "$BUILD_DIR"
rm -f "$BUILD_DIR"/qemu_uart1.log "$BUILD_DIR"/qemu_uart2.log "$BUILD_DIR"/qemu_uart3.log \
  "$BUILD_DIR"/qemu_shot.ppm
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp/home -v "$PWD":/pebbleos -w /pebbleos \
  "$IMAGE" bash -lc "
  mkdir -p \$HOME
  $SETUP
  git config --global --add safe.directory /pebbleos
  if ! grep -q '^CMAKE_HOME_DIRECTORY' $BUILD_DIR/CMakeCache.txt 2>/dev/null ||
     ! grep -qE '^CONFIG_MINIMED_SAKE=y' $BUILD_DIR/.config 2>/dev/null; then
    pbl configure -b $BUILD_DIR --board qemu_emery -DCONFIG_MINIMED_SAKE=y >/dev/null ||
      { echo 'CONFIGURE FAILED'; exit 1; }
  fi
  pbl build -b $BUILD_DIR >$BUILD_DIR/qemu_build.log 2>&1 ||
    { echo 'BUILD FAILED'; grep -E 'error:|undefined' $BUILD_DIR/qemu_build.log | head; exit 1; }
  cmake --build $BUILD_DIR --target qemu_image_micro qemu_image_spi >/dev/null 2>&1 ||
    { echo 'IMAGE FAILED'; exit 1; }
  rm -f $BUILD_DIR/qemu-mon.sock
  qemu-pebble -rtc base=localtime -monitor unix:$BUILD_DIR/qemu-mon.sock,server=on,wait=off \
    -serial file:$BUILD_DIR/qemu_uart1.log -serial file:$BUILD_DIR/qemu_uart2.log \
    -serial file:$BUILD_DIR/qemu_uart3.log \
    -machine pebble-emery,audiodev=snd0 -audiodev none,id=snd0 -kernel $BUILD_DIR/pebbleos.elf \
    -drive if=mtd,format=raw,file=$BUILD_DIR/qemu_spi_flash.bin -display none &
  QPID=\$!
  sleep $SECS
  python3 -c \"import socket,time; s=socket.socket(socket.AF_UNIX); s.connect('$BUILD_DIR/qemu-mon.sock'); s.sendall(b'screendump $BUILD_DIR/qemu_shot.ppm\n'); time.sleep(3)\"
  if kill -0 \$QPID 2>/dev/null; then echo 'QEMU still running after ${SECS}s'; else echo 'QEMU EXITED EARLY'; fi
  kill \$QPID 2>/dev/null; wait \$QPID 2>/dev/null || true
"
# The console serial carries PULSE-framed binary; strings pulls out the log text.
TEXT=$(strings -n 6 "$BUILD_DIR"/qemu_uart1.log "$BUILD_DIR"/qemu_uart2.log "$BUILD_DIR"/qemu_uart3.log)
echo "--- MiniMed lines"
grep -E 'minimed|fake pump' <<<"$TEXT" | tail -20 || true
echo "--- problems"
if grep -iE 'ASSERT|CROAK|hard ?fault|watchdog|stuck|queue full|took [0-9]+ ms' <<<"$TEXT"; then
  echo 'FAIL: see above'
  exit 1
fi
BOOTS=$(grep -c 'Firmware version' <<<"$TEXT" || true)
if [ "$BOOTS" != 1 ]; then
  echo "FAIL: booted $BOOTS times"
  exit 1
fi
grep -q 'fake pump: link up' <<<"$TEXT" || { echo 'FAIL: the MiniMed link never came up'; exit 1; }
grep -q 'minimed: subscribed IDD Status Changed' <<<"$TEXT" ||
  { echo 'FAIL: the session setup chain did not finish'; exit 1; }
for cmd in snooze confirm; do
  grep -q "minimed: annunc probe $cmd -> " <<<"$TEXT" ||
    { echo "FAIL: the Snooze/Confirm probe got no answer for $cmd"; exit 1; }
done
echo 'PASS'
