# Host verification for the on-watch SAKE C port. No watch / no ARM toolchain.
#
#   make        # build + run all known-answer + captured-trace checks
#
# Compiles the SAME module source that the firmware compiles (all of
# src/fw/services/minimed/core, which has no PebbleOS or NimBLE dependency) so
# there is a single source of truth. -DSAKE_TEST_HOOKS exposes the internal CMAC / SeqCrypt for
# the KATs. See ../../SPIKE-HANDOFF.md (Spike 2).

MOD := ../../src/fw/services/minimed/core
CFLAGS := -std=c99 -Wall -Wextra -O2 -DSAKE_TEST_HOOKS -I$(MOD)
SRC := $(wildcard $(MOD)/*.c) test_sake.c

test_sake: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ -lm

.PHONY: run clean
run: test_sake
	./test_sake

clean:
	rm -f test_sake replay

# Replay a pump event log through the history decoding and the predictor:
#   ./export_history_db.py <history.db> > history.hex && make replay
replay: replay.c $(MOD)/minimed_history.c $(MOD)/minimed_predict.c
	$(CC) $(CFLAGS) -I$(MOD) replay.c $(MOD)/minimed_history.c $(MOD)/minimed_predict.c -o replay -lm
	./replay history.hex
