# Host verification for the on-watch SAKE C port. No watch / no ARM toolchain.
#
#   make        # build + run all known-answer + captured-trace checks
#
# Compiles the SAME module source that the firmware compiles
# (src/bluetooth-fw/nimble/minimed_sake_{crypto,aes}.c) so there is a single
# source of truth. -DSAKE_TEST_HOOKS exposes the internal CMAC / SeqCrypt for
# the KATs. See ../../SPIKE-HANDOFF.md (Spike 2).

MOD := ../../src/bluetooth-fw/nimble
CFLAGS := -std=c99 -Wall -Wextra -O2 -DSAKE_TEST_HOOKS -I$(MOD)
SRC := $(MOD)/minimed_sake_aes.c $(MOD)/minimed_sake_crypto.c $(MOD)/minimed_annunciation.c $(MOD)/minimed_iob.c $(MOD)/minimed_graph.c $(MOD)/minimed_idd_flags.c $(MOD)/minimed_status.c test_sake.c

test_sake: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@

.PHONY: run clean
run: test_sake
	./test_sake

clean:
	rm -f test_sake
