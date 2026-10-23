# ADR 0002: Length-prefixed, versioned, fixed-header framing

Status: accepted

## Context

TCP is a byte stream with no message boundaries. Our first sketch used a single
command byte followed by command-specific fields. That works for a demo but
cannot be extended or debugged well.

## Decision

Every frame has a fixed 16-byte big-endian header: magic, version, kind, seq,
cmd, status, flags, length, followed by `length` payload bytes. Details in
docs/protocol.md.

## Why each field exists

- `length`: the receiver always knows how much to read, and can *skip* a frame
  it does not understand.
- `magic`: fail fast on a wrong or desynchronised stream.
- `version` at a fixed offset: mismatches are detectable forever.
- `seq`: pairs responses with requests, enabling pipelining later.
- `status` in the header: every response reports success or failure uniformly.
- `flags` (must be 0): reserved space with a safe failure mode.
- `kind`: lets the stub push events without being asked.

## Consequences

- (+) Extensible without breaking old peers; testable with golden vectors.
- (+) Bounded `length` defends the console against hostile or buggy input.
- (−) 16 bytes of overhead per frame. Irrelevant next to network latency.
- (−) No checksum: TCP already provides one. Serial transports (USB Gecko) will
  need one; that is what a `flags` bit would announce.
