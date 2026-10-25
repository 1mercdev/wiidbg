# WDP: the Wii Debug Protocol (wire version 1)

> Status: HELLO and PING are implemented (Phase 1). Memory commands are next.

This document is the **specification**. `common/wdp.[ch]` (C) and
`client/wiidbg/frame.py` (Python) are two implementations of it, and
`tests/vectors.txt` is the shared test data that keeps them identical.
If code and this document disagree, that is a bug: fix one of them.

Why a custom protocol, and why this shape? See
[ADR 0001](adr/0001-protocol-architecture.md) and
[ADR 0002](adr/0002-length-prefixed-framing.md).

## 1. Concepts

WDP runs over a reliable byte stream (TCP in Phase 1; USB Gecko serial later).
A stream has **no message boundaries**: two `send()` calls can arrive as one
`recv()`, and one `send()` can arrive as several. Every message therefore
starts with a fixed-size **header** that says how many payload bytes follow.

Three kinds of frame:

| kind | value | direction | meaning |
|------|-------|-----------|---------|
| REQUEST  | 0 | PC → Wii | "do this". Carries a `seq` chosen by the PC. |
| RESPONSE | 1 | Wii → PC | Answer to the request with the **same `seq`**. Carries a `status`. |
| EVENT    | 2 | Wii → PC | Unsolicited: a watchpoint hit, a log line, ... `cmd` holds the event id. |

Every request gets exactly one response. Events are never answered.

## 2. Header (16 bytes, all integers big-endian)

| offset | size | field   | notes |
|-------:|-----:|---------|-------|
| 0  | 2 | `magic`   | `0x5744` (ASCII "WD"). Catches "this isn't WDP" immediately. |
| 2  | 1 | `version` | wire-format version. This document describes **1**. |
| 3  | 1 | `kind`    | 0, 1 or 2 as above. Anything else is invalid. |
| 4  | 2 | `seq`     | Pairs responses to requests. Events use their own stub-side counter. |
| 6  | 2 | `cmd`     | Command id (requests/responses) or event id (events). |
| 8  | 2 | `status`  | Result code in responses; **0** in requests and events. |
| 10 | 2 | `flags`   | Reserved. **Must be 0** in version 1. |
| 12 | 4 | `len`     | Number of payload bytes after the header. At most `0x4000`. |

Why these choices:

- **Big-endian** is the Wii's native order, so the stub never swaps bytes; the
  PC side pays the (tiny) conversion cost.
- **16 bytes, 4-aligned fields**: easy to read in a hex dump, cheap to parse.
- **`magic` and `version` at offsets 0–2 never move**, in any version. That is
  what lets *any* receiver tell "wrong protocol" or "wrong version" even if it
  cannot parse the rest.
- **`flags` must be 0** and receivers reject non-zero flags they do not know.
  A future flag could change how the payload is read (compression, checksum);
  ignoring it would silently misparse, so we fail loudly instead.
- **`len` is bounded** (16 KiB). A receiver checks it *before* reading or
  allocating, so a bogus 4 GiB length from the network cannot crash the console.

### Worked example: PING with payload `"ABCD"`

Request, `seq = 1`, `cmd = 2` (PING), payload 4 bytes:

```
57 44  01  00  00 01  00 02  00 00  00 00  00 00 00 04   41 42 43 44
magic  ver kind seq    cmd    status flags  len           payload "ABCD"
```

Response: identical, except `kind = 01` and the payload is echoed back:

```
57 44  01  01  00 01  00 02  00 00  00 00  00 00 00 04   41 42 43 44
```

These exact header bytes appear as the first two vectors in
`tests/vectors.txt` and are checked by both test suites.

## 3. Versioning and compatibility

There are three independent mechanisms, from coarse to fine:

1. **`version` byte** (wire format). Bumped only if the header layout or
   framing rules change, i.e. if old and new software could not even parse each
   other's frames. We intend this to be rare.
2. **Capabilities** (feature detection). The `HELLO` reply carries a bitmask of
   what this stub build supports (memory read, memory write, watchpoints, ...).
   A client asks "do you support X?" instead of comparing version numbers.
3. **Unknown commands are an error response, not a disconnect.** A newer client
   talking to an older stub gets `ERR_UNKNOWN_CMD` and can degrade gracefully.

Handshake policy: clients **should** send `HELLO` first to learn the stub's
version and capabilities, but stubs do **not require** it: every frame already
carries the `version` byte, so each frame is checked on its own. (Phase 0 of
this document said "must"; the implementation turned out simpler and just as
safe without enforcing it, so the spec follows the code.)

If a frame's `version` is one the stub does not speak, the stub **answers once
and then closes the connection**:

- the reply has `status = ERR_BAD_VERSION`, the request's `seq` and `cmd`, and a
  1-byte payload holding the version the stub *does* speak;
- it then hangs up, because with an unknown version it cannot trust `len`, so it
  could not find the next frame boundary anyway.

The `version` and `magic` bytes are checked **before** any other header field,
straight from the raw bytes, so a future version that reuses the other fields
differently can still be diagnosed. (This is verified by a test that sends a
wrong version *and* garbage flags and expects the version error.)

While the project is pre-1.0 the version stays at 1 and incompatible changes
to *commands* are allowed; the header itself is considered frozen.

## 4. Status codes

| value | name | meaning |
|------:|------|---------|
| 0 | `OK` | success |
| 1 | `ERR_UNKNOWN_CMD` | the stub does not know this command |
| 2 | `ERR_BAD_PAYLOAD` | payload malformed for this command |
| 3 | `ERR_BAD_VERSION` | header `version` not supported |
| 4 | `ERR_RANGE` | address range not allowed (see memory map) |
| 5 | `ERR_TOO_BIG` | request or reply would exceed the max payload |
| 6 | `ERR_BUSY` | cannot service this right now |
| 7 | `ERR_UNSUPPORTED` | known command, but not built into this stub |
| 8 | `ERR_INTERNAL` | stub bug |

## 5. Command id allocation

Ids are grouped so a new feature never needs to renumber an old one.

| range | area |
|-------|------|
| `0x0000–0x00FF` | core (handshake, ping) |
| `0x0100–0x01FF` | memory access |
| `0x0200–0x02FF` | watchpoints and breakpoints |
| `0x0300–0x03FF` | registers and execution control |
| `0x1000–0x1FFF` | events (stub → PC) |
| `0x8000–0xFFFF` | experimental / vendor; never standardised |

Defined so far (implemented in **Phase 1**):

| id | name | request payload | response payload |
|---:|------|-----------------|------------------|
| `0x0001` | `HELLO` | empty (a non-empty payload gets `ERR_BAD_PAYLOAD`) | see below |
| `0x0002` | `PING`  | any bytes (≤ max) | the same bytes |

`HELLO` response payload (all big-endian):

| offset | size | field |
|-------:|-----:|-------|
| 0 | 1 | `stub_major` |
| 1 | 1 | `stub_minor` |
| 2 | 2 | reserved: sent as 0, **ignored** by receivers |
| 4 | 4 | `capabilities` bitmask (no bits defined yet) |
| 8 | 4 | `max_payload` the stub accepts |
| 12 | 2 | `build_len`, at most 31 |
| 14 | n | `build` string, UTF-8, `build_len` bytes (e.g. `0.1.0`) |

Forward compatibility rules for this payload:

- **Receivers ignore any bytes after the build string.** A future stub may append
  fields; today's clients must keep working.
- The build string is limited to 31 bytes so it fits a fixed 32-byte buffer with
  a terminator on the Wii (no allocation). A longer `build_len` is an error.
- A payload shorter than its own fields require is an error (`SHORT`).

These rules are pinned by the `hello` and `hello_invalid` lines of
`tests/vectors.txt`, which both the C and Python codecs must satisfy.

`PING` is a plain echo: the reply carries exactly the request's payload. It
exists to check the link and to measure round-trip time and throughput.

## 6. Server behaviour (Phase 1)

How the stub treats connections. These are policies, not wire format, and may
evolve (see ADR 0005 for the structure that makes them easy to change).

- Listens on TCP port **4242** (`WDP_DEFAULT_PORT`).
- **One client at a time.** A debugger session is exclusive.
- **Newest connection wins.** A new connection replaces the current one. A
  stale session (Wi-Fi dropped without a clean close) can then never lock you
  out. Trade-off: with no authentication, anyone who can reach the port can take
  over the session.
- **One request at a time.** The stub answers each request before reading the
  next. A client may send several requests back to back ("pipelining"); the
  extra bytes simply wait in the network buffer, and replies come back in order.
  This bounds the stub's memory however fast a client sends.
- **Unknown command** → `ERR_UNKNOWN_CMD`, connection stays open.
- **Framing errors** (bad magic, unknown kind, non-zero flags, oversize length,
  or a client sending a RESPONSE/EVENT frame) → the stub closes the connection
  *without* replying. There is no way to resynchronise a TCP stream after
  garbage, and replying to an unknown protocol would be pointless.

## 7. Rules for implementers

- Decode with explicit shifts or `struct`, never by casting a buffer to a struct.
- Validate the header, then `len`, **before** reading the payload.
- Drop the connection on a framing error (bad magic, kind, flags, oversize).
  On a TCP stream there is no way to resynchronise after garbage. The one
  exception is a wrong `version`, which is answered first (section 3).
- A stub handles one request at a time and never blocks the main loop for long.
- Security: WDP v1 has **no authentication or encryption**. Anyone who can
  reach the port can read and write the console's memory. See README, "Security".
