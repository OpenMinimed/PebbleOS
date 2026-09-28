# SPDX-FileCopyrightText: 2026 Pal Marci
# SPDX-License-Identifier: Apache-2.0

# core/ is plain C with no PebbleOS or NimBLE dependency: the protocol parsers, the SAKE crypto
# and the models. It is what a standalone library or a watchapp would reuse as is, and what the
# host tests in tools/minimed_sake_hosttest build. Everything else here is the firmware service.
pbl_include_directories(. core)

file(GLOB sources CONFIGURE_DEPENDS *.c core/*.c)
pbl_library()
pbl_library_sources(${sources})

pbl_add_subdirectory_ifdef(CONFIG_MINIMED_STUB_VOICE stubs)
