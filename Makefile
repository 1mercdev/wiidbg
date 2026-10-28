# SPDX-License-Identifier: GPL-3.0-only
#
# Top-level Makefile. Two kinds of targets:
#   * HOST targets (test, test-c, test-py) run on your PC, no Wii tools needed.
#   * WII targets (stub, target) need devkitPPC; see README.

CC      ?= gcc
CFLAGS_HOST := -std=c11 -pedantic -Wall -Wextra -Werror -g -fsanitize=address,undefined
BIN     := tests/host/bin

.PHONY: test test-c test-py stub target clean hostserver

test: test-c test-py

# Compiles the portable C (common/) for the PC and runs it against the shared
# vectors. -fsanitize makes memory bugs and undefined behaviour fail loudly.
test-c: $(BIN)/test_wdp $(BIN)/test_wdlog $(BIN)/test_wmem $(BIN)/test_conn $(BIN)/hostserver
	$(BIN)/test_wdp tests/vectors.txt
	$(BIN)/test_wdlog
	$(BIN)/test_wmem
	$(BIN)/test_conn

# A fake Wii stub that runs on the PC (see tests/host/hostserver.c).
hostserver: $(BIN)/hostserver
	tests/host/bin/hostserver

$(BIN)/test_wdp: tests/host/test_wdp.c common/wdp.c common/wdp.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_wdp.c common/wdp.c -o $@

$(BIN)/test_wdlog: tests/host/test_wdlog.c common/wdlog.c common/wdlog.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_wdlog.c common/wdlog.c -o $@

$(BIN)/test_wmem: tests/host/test_wmem.c common/wmem.c common/wmem.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_wmem.c common/wmem.c -o $@

$(BIN)/test_conn: tests/host/test_conn.c common/wdp_conn.c common/wdp_conn.h common/wdp.c common/wdp.h common/wmem.c common/wmem.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) tests/host/test_conn.c common/wdp_conn.c common/wdp.c common/wmem.c -o $@

HOSTSERVER_SRC := tests/host/hostserver.c tests/host/wnet_posix.c tests/host/wmem_fake.c \
                  common/wdp_server.c common/wdp_conn.c common/wdp.c common/wdlog.c common/wmem.c
$(BIN)/hostserver: $(HOSTSERVER_SRC) common/wdp_server.h common/wnet.h common/wdp_conn.h common/wdp.h common/wdlog.h common/wmem.h tests/host/wmem_fake.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS_HOST) $(HOSTSERVER_SRC) -o $@

test-py: $(BIN)/hostserver
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
