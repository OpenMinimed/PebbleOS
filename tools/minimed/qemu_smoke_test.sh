#!/bin/bash
# SPDX-FileCopyrightText: 2026 Pal Marci
# SPDX-License-Identifier: Apache-2.0
#
# Boot the MiniMed firmware in QEMU and check that it survives: the MiniMed task starts, the
# stand-in pump link (src/bluetooth-fw/qemu/minimed_fake_pump.c) comes up, and the watch neither
# asserts nor reboots. Not a pump simulation -- a crash test for the task integration.
#
#   tools/minimed/qemu_smoke_test.sh [seconds]     (default 200)
#
# Configures build/ for qemu_emery with CONFIG_MINIMED_SAKE (the next minimed-build.sh run
# reconfigures for the real board). Uses the pebbleos-minimed:local image if it exists (see
# tools/minimed/Dockerfile), else the CI image plus the missing packages. Leaves the serial logs in
# build/qemu_uart{1,2,3}.log and a screenshot in build/qemu_shot.ppm.

set -e
cd "$(dirname "$0")/../.."
SECS=${1:-200}

if docker image inspect pebbleos-minimed:local >/dev/null 2>&1; then
  IMAGE=pebbleos-minimed:local
  SETUP=':'
else
  IMAGE=ghcr.io/coredevices/pebbleos-docker:v6
  SETUP='git config --global --add safe.directory /pebbleos
    (apt-get update -qq && apt-get install -y -qq libsdl2-2.0-0) >/dev/null 2>&1
    pip install -r requirements.txt >/dev/null 2>&1
    export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:/opt/pebbleos-sdk/qemu/bin:$PATH'
fi

rm -f build/qemu_uart1.log build/qemu_uart2.log build/qemu_uart3.log build/qemu_shot.ppm
docker run --rm -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos "$IMAGE" bash -lc "
  $SETUP
  if ! grep -q \"BOARD = 'qemu_emery'\" build/c4che/_cache.py 2>/dev/null ||
     ! grep -qE '^CONFIG_MINIMED_SAKE = (1|True)' build/c4che/_cache.py; then
    ./waf configure --board qemu_emery -DCONFIG_MINIMED_SAKE=y >/dev/null
  fi
  ./waf build >build/qemu_waf.log 2>&1 || { echo 'BUILD FAILED'; grep -E 'error:|undefined' build/qemu_waf.log | head; exit 1; }
  ./waf qemu_image_micro qemu_image_spi >/dev/null 2>&1 || { echo 'IMAGE FAILED'; exit 1; }
  rm -f build/qemu-mon.sock
  qemu-pebble -rtc base=localtime -monitor unix:build/qemu-mon.sock,server=on,wait=off \
    -serial file:build/qemu_uart1.log -serial file:build/qemu_uart2.log -serial file:build/qemu_uart3.log \
    -machine pebble-emery,audiodev=snd0 -audiodev none,id=snd0 -kernel build/pebbleos.elf \
    -drive if=mtd,format=raw,file=build/qemu_spi_flash.bin -display none &
  QPID=\$!
  sleep $SECS
  python3 -c \"import socket,time; s=socket.socket(socket.AF_UNIX); s.connect('build/qemu-mon.sock'); s.sendall(b'screendump build/qemu_shot.ppm\n'); time.sleep(3)\"
  if kill -0 \$QPID 2>/dev/null; then echo 'QEMU still running after ${SECS}s'; else echo 'QEMU EXITED EARLY'; fi
  kill \$QPID 2>/dev/null; wait \$QPID 2>/dev/null || true
"
# The console serial carries PULSE-framed binary; strings pulls out the log text.
TEXT=$(strings -n 6 build/qemu_uart1.log build/qemu_uart2.log build/qemu_uart3.log)
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
echo 'PASS'
