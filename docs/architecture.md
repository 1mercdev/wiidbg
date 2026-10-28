# Architecture

How the pieces fit, and why they are split this way. Decisions behind it:
[ADR 0001](adr/0001-protocol-architecture.md),
[ADR 0003](adr/0003-logging-architecture.md),
[ADR 0005](adr/0005-sans-io-core-and-platform-boundary.md).

## The layers

Code is stacked in layers. **A layer only calls the one below it**, and only the
bottom layer knows which platform it runs on.

```
 PC side (Python)                         Wii side (C)
 ────────────────                         ────────────
 cli.py        user commands              stub/source/main.c   boot, video, main loop
 conn.py       seq numbers, errors        ─────────────────────────────────────────────
 messages.py   command ids, HELLO         common/wdp_server.c  accept / recv / feed / send loop
 frame.py      16-byte header             common/wdp_conn.c    protocol state machine (NO I/O)
                                          common/wdp.c         header + HELLO codecs
                                          ─────────────────────────────────────────────
                       TCP                common/wnet.h        6-function network boundary
                  ◄──────────────►        ├─ stub/source/wnet_wii.c       libogc (the console)
                                          └─ tests/host/wnet_posix.c      BSD sockets (your PC)

                                          common/wmem.c        memory ACCESS POLICY (which addresses)
                                          common/wmem.h        2-function memory backend interface
                                          ├─ stub/source/wmem_wii.c       real RAM + cache maintenance
                                          └─ tests/host/wmem_fake.c       plain buffers (your PC)
```

| layer | knows about | does NOT know about |
|-------|-------------|---------------------|
| `wdp` codec | byte layout of headers/payloads | sockets, state, the Wii |
| `wdp_conn` | what each command means, one connection's state | where bytes come from |
| `wdp_server` | sockets (through `wnet.h`), sessions | the platform's socket API |
| `wnet_*` | the platform's socket API | the protocol |
| `wmem` policy | which addresses are allowed | how memory is accessed |
| `wmem_*` backends | how bytes are read and written (caches) | whether a range is allowed |

The payoff: the three middle layers are plain portable C. They are compiled and
tested on your PC with ordinary tools, and then run unchanged on the console.
Only `wnet_wii.c` (and the video/controller code in `main.c`) is Wii-specific.

## Life of one request: PING

```
client                          stub
  │  Connection.ping(b"ABCD")
  │    frame.Header(...).encode()  → 16-byte header + 4 payload bytes
  │  ───────────── TCP ─────────────►
  │                                  wdp_server_poll()              (main loop, ~1000×/s)
  │                                   wnet_recv()      bytes arrive
  │                                   wdp_conn_feed()  header complete → validate magic,
  │                                                    version, kind, flags, length
  │                                                    payload complete → handle_request()
  │                                                    → queue 16+4 byte reply in c->tx
  │                                   wdp_conn_out()   server asks "what should I send?"
  │                                   wnet_send()      bytes leave
  │  ◄──────────── TCP ──────────────
  │    check kind, seq, cmd, status; return payload
```

Two details worth noticing:

1. `wdp_conn_feed()` is handed bytes in whatever pieces TCP delivered them. It can
   stop in the middle of a header and resume later. A test feeds a request one
   byte at a time and checks the reply is identical to feeding it all at once.
2. While a reply is waiting to be sent, `feed` takes no more input
   (*backpressure*). A client that blasts requests cannot make the stub allocate
   more memory: the excess waits in the TCP buffer.

## How we test without a Wii

| test | what runs | proves |
|------|-----------|--------|
| `tests/host/test_wdp.c` | codec vs `tests/vectors.txt` | C encodes/decodes the spec'd bytes |
| `client/tests/test_frame.py`, `test_messages.py` | Python codec vs the **same** vectors | Python agrees with C |
| `tests/host/test_wmem.c` | the address policy, at every region boundary | a bad address is refused, including 32-bit wraparound |
| `tests/host/test_conn.c` | state machine, bytes in/out, no sockets | protocol logic, edge cases, limits. With a recording fake memory it also proves a refused request never reaches memory |
| `tests/host/test_wdlog.c` | logging core | ring buffers, merging, overflow |
| `client/tests/test_integration.py` | real Python client ↔ `hostserver` (with fake RAM) over TCP | everything above, together: round trips, chunking, region edges, partial writes |
| `client/tests/test_cli.py` | CLI parsing and hex dump | the user-facing text is right |

`tests/host/hostserver.c` is the **same server loop as the stub**, on PC sockets.
You can also run it by hand and point the client at it:

```sh
make hostserver
tests/host/bin/hostserver            # prints: listening 127.0.0.1:<port>
python -m wiidbg 127.0.0.1:<port> hello      # run from client/
```

What this does **not** prove: that `wnet_wii.c` matches libogc's real behaviour,
and anything about IOS. That is what Dolphin (and later real hardware) is for.

## Recipe: adding a command (the pattern for Phase 2 onwards)

1. **Spec** (`docs/protocol.md`): id from the right range, request/response
   payload tables, error cases, capability bit.
2. **Constants**: `enum wdp_cmd` in `common/wdp.h` and `Cmd` in `messages.py`.
3. **Payload codecs** (if it has structured payloads): C in `common/wdp.c`,
   Python in `messages.py`, and **shared vectors** in `tests/vectors.txt`.
4. **Handler**: a `case` in `handle_request()` in `common/wdp_conn.c`. Hardware
   access goes behind a small interface (like `wnet.h` and `wmem.h`) so it stays
   testable. Do every validity check before the first side effect, and test that a
   refused request leaves the fake backend untouched. `handle_mem_write` is the
   model to copy.
5. **Tests first or alongside**: a case in `test_conn.c`, an end-to-end case in
   `test_integration.py` against `hostserver`.
6. **Client**: a method on `Connection`, then a CLI subcommand.
7. **Capability**: set the bit in the stub's HELLO so clients can detect the feature.
