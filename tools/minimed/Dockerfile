# SPDX-FileCopyrightText: 2026 Pal Marci
# SPDX-License-Identifier: Apache-2.0
#
# Build and emulator image for the MiniMed fork: the official PebbleOS CI image with what every
# run otherwise installs by hand -- the Python requirements, and SDL2, without which the bundled
# qemu-pebble does not start. Build it from the repository root:
#
#   docker build -t pebbleos-minimed:local -f tools/minimed/Dockerfile .
#
# then use it wherever ghcr.io/coredevices/pebbleos-docker:v6 is used, e.g.
# tools/minimed/qemu_smoke_test.sh.

FROM ghcr.io/coredevices/pebbleos-docker:v6

RUN apt-get update \
    && apt-get install -y --no-install-recommends libsdl2-2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# requirements.txt installs the repo's own tools/libs packages in editable mode. Install them from
# /pebbleos, where the checkout is mounted at run time: the mount then shadows this copy, and the
# editable links resolve to the live sources.
COPY requirements.txt /pebbleos/requirements.txt
COPY tools/libs /pebbleos/tools/libs
# The CI image keeps its Python in /opt/venv (on PATH only in login shells).
RUN cd /pebbleos && /opt/venv/bin/pip install -U pip && /opt/venv/bin/pip install -r requirements.txt

ENV PATH=/opt/venv/bin:/opt/pebbleos-sdk/arm-none-eabi/bin:/opt/pebbleos-sdk/qemu/bin:$PATH
RUN git config --system --add safe.directory /pebbleos

WORKDIR /pebbleos
