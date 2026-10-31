# ADR 0003: Ring buffers + pluggable sinks, with a separate exception path

Status: accepted. Partly superseded by [ADR 0007](0007-no-usb-gecko.md): the USB Gecko
sink and crash path mentioned below are no longer planned.

## Context

We must log from exception handlers and from code that may be failing, on a
console whose network stack runs on a separate CPU. Direct `printf` is unsafe
there and tangles every module with output details.

## Decision

Producers copy fixed-size records into RAM rings; `wdlog_flush()` in the main
loop delivers them to registered sinks (console now; network, USB Gecko, file
later). Exception handlers use a separate ring and a libc-free API. See
docs/logging.md.

## Consequences

- (+) Safe anywhere, never blocks, new outputs added without touching callers.
- (+) Fully host-testable: the core is portable C.
- (+) Loss is reported, never silent.
- (−) Logs appear slightly after the event (at the next flush), and a hard crash
  can lose records not yet flushed. Mitigation: the synchronous USB Gecko crash
  path in Phase 4.
- (−) Fixed 80-byte messages. Deliberate: bounded memory beats flexibility here.
