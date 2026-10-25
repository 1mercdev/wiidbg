# ADR 0005: Sans-I/O protocol core, six-function platform boundary

Status: accepted

## Context

Phase 1 must send and receive bytes over the Wii's network stack, which is only
reachable through libogc and cannot be run on a PC. We want almost all of the
logic to be testable without a console or an emulator, and we want future
platforms (a different console SDK, a PC simulator) to be cheap to add.

## Options

1. **Write the server directly against libogc.** Fastest to type; testable only
   by running the Wii build. Protocol bugs and network bugs are tangled.
2. **Sans-I/O core + thin platform layer.** (chosen) The protocol state machine
   (`wdp_conn`) never touches a socket: bytes in, bytes out. A server loop
   (`wdp_server`) moves bytes using six functions (`wnet.h`), implemented once
   per platform.

## Decision

Option 2. All platform-specific networking lives in two small files:
`stub/source/wnet_wii.c` and `tests/host/wnet_posix.c`.

## Consequences

- (+) The protocol logic and the real server loop are tested on the PC with
  sanitizers, including awkward cases (byte-at-a-time input, hostile clients).
- (+) A libogc surprise is fixed in one file, and cannot be confused with a
  protocol bug.
- (+) `hostserver` lets the Python client be developed with no emulator at all.
- (−) A little indirection and a few more files than a direct implementation.
- (−) The host tests prove the logic, not that `wnet_wii.c` matches libogc's real
  behaviour; that part still needs Dolphin and real hardware.

## Related decisions made in the same change

- **One client, newest wins** (see docs/protocol.md section 6): trades a little
  security for never being locked out by a half-dead connection. Revisit when
  authentication is added.
- **One request at a time with backpressure**: bounded memory beats throughput
  for now. Revisit if throughput measurements say otherwise.
- **Non-blocking sockets, polled from the main loop**: a blocking `recv` would
  freeze the controller and the log. The alternative, a separate thread, adds
  concurrency we do not need yet.
