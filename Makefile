# SPDX-License-Identifier: GPL-3.0-only
#
# Top-level Makefile. Two kinds of targets:
#   * HOST targets (test, test-c, test-py) run on your PC, no Wii tools needed.
#   * WII targets (stub, target) need devkitPPC; see README.

CC      ?= gcc
CFLAGS_HOST := -std=c11 -pedantic -Wall -Wextra -Werror -g -fsanitize=address,undefined
BIN     := tests/host/bin

.PHONY: test test-c test-py stub target clean

test: test-c test-py

# Compiles the portable C (common/) for the PC and runs it against the shared
# vectors. -fsanitize makes memory bugs and undefined behaviour fail loudly.
test-c: $(BIN)/test_wdp $(BIN)/test_wdlog
	$(BIN)/test_wdp tests/vectors.txt
	$(BIN)/test_wdlog

$(BIN)/test_wdp: tests/host/test_wdp.c common/wdp.c common/wdp.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_wdp.c common/wdp.c -o $@

$(BIN)/test_wdlog: tests/host/test_wdlog.c common/wdlog.c common/wdlog.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_wdlog.c common/wdlog.c -o $@

test-py:
	cd client && python3 -m unittest discover -s tests -v

# Wii builds (need DEVKITPRO / DEVKITPPC in the environment).
stub:
	$(MAKE) -C stub

target:
	$(MAKE) -C target

clean:
	rm -rf $(BIN)
	-$(MAKE) -C stub clean
	-$(MAKE) -C target clean
