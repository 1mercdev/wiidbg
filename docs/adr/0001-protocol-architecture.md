# ADR 0001: Native binary protocol (WDP) plus a GDB bridge on the PC

Status: accepted (revisit after Phase 3)

## Context

The tool is meant for everyone, not only for us. The obvious alternative to
inventing a protocol is the **GDB Remote Serial Protocol (RSP)**, which gives
users `gdb`, and with adapters Ghidra and others, for free.

## Options

1. **Custom binary protocol only.** Small and fast on the Wii; every user must
   use our client.
2. **GDB RSP on the Wii only.** Instant ecosystem; but RSP is ASCII/hex (more
   parsing on a 729 MHz CPU), and Wii-specific features (cache control, IOS
   tracing, a streaming event feed) fit it awkwardly.
3. **Custom protocol on the Wii, plus a GDB RSP *bridge* on the PC.** (chosen)

## Decision

Option 3. The Wii speaks WDP (compact, versioned, extensible). A small PC-side
program translates RSP to WDP so stock `gdb-multiarch` works. The bridge is
written in Python, where it is cheap to maintain.

## Consequences

- (+) The stub stays small, simple and fast; exception-context code is minimal.
- (+) Everyone gets a familiar frontend (gdb), and we are not locked to our own
  client. Wii-specific features remain first-class in WDP.
- (+) Protocol evolution is under our control (see ADR 0002 and docs/protocol.md).
- (−) Two layers to maintain, and RSP ↔ WDP mismatches to handle. Mitigated by
  doing the bridge late (Phase 5), once WDP is stable.
- (−) Slightly more to learn. That is acceptable: this is also a learning project.
