# WDP: the Wii Debug Protocol (wire version 1)

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

Handshake policy: a client must send `HELLO` first. If the stub does not
support the header `version`, it still answers (with `ERR_BAD_VERSION` and the
version it *does* speak) rather than hanging up, which is why the C decoder
does **not** reject unknown versions itself, leaving that policy to the caller.

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

Defined so far (**Phase 0**; handlers arrive in Phase 1):

| id | name | request payload | response payload |
|---:|------|-----------------|------------------|
| `0x0001` | `HELLO` | empty | see below |
| `0x0002` | `PING`  | any bytes (≤ max) | the same bytes |

`HELLO` response payload (all big-endian):

| offset | size | field |
|-------:|-----:|-------|
| 0 | 1 | `stub_major` |
| 1 | 1 | `stub_minor` |
| 2 | 2 | reserved, 0 |
| 4 | 4 | `capabilities` bitmask (none defined yet) |
| 8 | 4 | `max_payload` the stub accepts |
| 12 | 2 | `build_len` |
| 14 | n | `build` string, UTF-8, `build_len` bytes (e.g. `0.0.1`) |

## 6. Rules for implementers

- Decode with explicit shifts or `struct`, never by casting a buffer to a struct.
- Validate the header, then `len`, **before** reading the payload.
- Drop the connection on a framing error (bad magic, kind, flags, oversize).
  On a TCP stream there is no way to resynchronise after garbage.
- A stub handles one request at a time and never blocks the main loop for long.
- Security: WDP v1 has **no authentication or encryption**. Anyone who can
  reach the port can read and write the console's memory. See README, "Security".
