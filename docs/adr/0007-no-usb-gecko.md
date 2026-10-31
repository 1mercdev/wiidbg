# ADR 0007: No dependency on a USB Gecko

Status: accepted. Amends [ADR 0003](0003-logging-architecture.md) (and a remark in
[ADR 0002](0002-length-prefixed-framing.md)).

## Context

Earlier plans used a USB Gecko (a serial adapter in the GameCube memory card slot)
as the one output that works even when the network is down: a synchronous crash
path for the logging system in Phase 4. The project owner does not own one, and it
is no longer sold new, so anyone following the project would be in the same
position.

## Decision

No feature, test or document depends on a USB Gecko. It is removed from the
roadmap and the docs.

## Consequences

- (+) Everything in the project works with a modded Wii, an SD card and Wi-Fi.
- (−) There is no way to see a crash that takes down the network or the screen at
  the same time. Mitigations, in logging.md: handlers designed to return rather than
  crash, libogc's on-screen crash dump with the build's map file, and flushing the
  log before risky steps.
- (−) A hard crash can lose log records that were not yet flushed.
- The logging system's sink interface does not care what the output is, so if a
  contributor with the hardware ever wants to add a sink, nothing else changes. It
  is simply not part of the plan.
