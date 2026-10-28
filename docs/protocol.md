# WDP: the Wii Debug Protocol (wire version 1)

> Status: HELLO, PING, MEM_READ and MEM_WRITE are implemented (Phases 1 and 2).

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

Defined so far:

| id | name | phase | request payload | response payload |
|---:|------|:-----:|-----------------|------------------|
| `0x0001` | `HELLO` | 1 | empty (a non-empty payload gets `ERR_BAD_PAYLOAD`) | see below |
| `0x0002` | `PING`  | 1 | any bytes (≤ max) | the same bytes |
| `0x0100` | `MEM_READ`  | 2 | `u32 addr`, `u32 len` | `len` bytes of memory |
| `0x0101` | `MEM_WRITE` | 2 | `u32 addr`, then the bytes to write | empty |

`HELLO` response payload (all big-endian):

| offset | size | field |
|-------:|-----:|-------|
| 0 | 1 | `stub_major` |
| 1 | 1 | `stub_minor` |
| 2 | 2 | reserved: sent as 0, **ignored** by receivers |
| 4 | 4 | `capabilities` bitmask (see "Capabilities" below) |
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


### Capabilities

A stub sets a bit only for what it can really do (the HELLO reply is built from
the configured backends, so it cannot claim a feature that is missing).

| bit | name | meaning |
|----:|------|---------|
| 0 | `MEM_READ`  | the stub can read memory |
| 1 | `MEM_WRITE` | the stub can write memory |

Without a capability the matching command answers `ERR_UNSUPPORTED`.

### Memory commands (Phase 2)

Background on caches and the memory map: [memory.md](memory.md). Design
decisions: [ADR 0006](adr/0006-memory-access.md).

**Allowed addresses.** Only the Wii's two RAM areas, through the CPU's normal
(cached) addresses. A range must lie entirely inside **one** of them.

| region | first byte | last byte | size |
|--------|-----------:|----------:|-----:|
| MEM1 | `0x80000000` | `0x817FFFFF` | 24 MiB |
| MEM2 | `0x90000000` | `0x93FFFFFF` | 64 MiB |

Everything else is `ERR_RANGE`: hardware registers (reading some has side
effects), the uncached mirrors, unmapped addresses (they would crash the stub).
A range that would run past `0xFFFFFFFF` is refused, not wrapped around.

**Byte order.** Memory bytes are transferred exactly as stored. The Wii is
big-endian, so a 32-bit value `0x01020304` appears as `01 02 03 04`. The protocol
never reinterprets memory contents. (Header and payload *fields*, such as `addr`,
are big-endian too, as everywhere in WDP.)

`MEM_READ` (`0x0100`)

| request | reply |
|---------|-------|
| `u32 addr`, `u32 len` (exactly 8 bytes) | `len` bytes, status `OK` |

Checks, **in this order** (clients may rely on it; the first failure wins):
`ERR_UNSUPPORTED` (no read capability) → `ERR_BAD_PAYLOAD` (request is not 8 bytes,
or `len` is 0) → `ERR_TOO_BIG` (`len` exceeds `max_payload`) → `ERR_RANGE` →
`ERR_INTERNAL` (the backend failed). Memory is touched only after every check
has passed.

`MEM_WRITE` (`0x0101`)

| request | reply |
|---------|-------|
| `u32 addr`, then 1 or more data bytes | empty, status `OK` |

The data is at most `max_payload - 4` bytes (4 bytes of the payload are the
address). Checks, in order: `ERR_UNSUPPORTED` → `ERR_BAD_PAYLOAD` (payload shorter
than 5 bytes) → `ERR_RANGE` → `ERR_INTERNAL`.

- **A request is all or nothing.** If any check fails, nothing is written.
- **A request whose payload exceeds `max_payload` is a framing error** (section 6):
  the header check rejects it before the command is even looked at, and the
  connection is closed. Clients must split large writes themselves.
- After a successful write the stub makes the new bytes visible to the rest of the
  console (data cache flushed, instruction cache invalidated), so patched code
  runs. Details in memory.md.

**Large transfers are the client's job.** The client splits a long read or write
into several requests, each within the `max_payload` the stub reported in HELLO
(never a hard-coded number). Splitting has one consequence worth knowing:

> A single request is applied in one step. A write split over **several**
> requests is **not atomic**: if a later request fails, the earlier ones have
> already happened, and a running program may execute half-patched code in
> between. Keep patches that must be atomic within one request.

The Python client reports this case as `PartialWriteError`, saying how many bytes
landed.

## 6. Server behaviour

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
- Security: WDP v1 has **no authentication or encryption**. Since Phase 2 that
  means anyone who can reach the port can read and **write the console's memory**,
  which is the same as running code on it. See README, "Security".
