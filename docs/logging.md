# Logging

Code: `common/wdlog.[ch]` (portable core) and `stub/source/log_sinks.c` (Wii sinks).
Decision record: [ADR 0003](adr/0003-logging-architecture.md).

## The problem

A debugger runs in the nastiest places: inside exception handlers, while the
program being debugged may have corrupted memory, with a network stack that
lives in a *different CPU* (the IOS coprocessor). Logging there with `printf`
can hang or corrupt the very thing you are trying to observe. And when something
goes wrong, the log is the only witness.

## The design in one sentence

**Producers never touch a device; they only copy a record into RAM. A single
place in the main loop later hands records to pluggable sinks.**

```
 normal code ──► ring[NORMAL] ──┐
                                 ├─► wdlog_flush() ─► sink: console (TV)
 exception handler ► ring[EXC] ──┘   (main loop)      sink: network / PC   (Phase 1+)
                                                       sink: USB Gecko      (Phase 4)
```

## Properties, and the test that proves each

| property | why it matters | test (`tests/host/test_wdlog.c`) |
|----------|----------------|----------------------------------|
| Logging never blocks | a full ring must not stall a handler | `overflow_is_dropped_and_reported` |
| Loss is never silent | a dropped line looks like "nothing happened" | same test: a synthetic WARN reports the count, once |
| Levels filter per sink | TV wants INFO, a file might want TRACE | `sink_level_filtering` |
| Long messages are truncated safely | no buffer overrun | `truncation_is_safe`, `exc_write_truncates...` |
| Two contexts, one time-ordered stream | exception logs interleave correctly | `exception_ring_merges_by_time` |
| Correct across timebase wraparound | the 32-bit tick wraps every ~70 s | `merge_survives_clock_wraparound` |

## Why two rings

The Wii's CPU has a single core, so the only concurrency is an exception
interrupting normal code at an arbitrary instruction. A queue is safe if each
variable has **one writer**. Giving normal code and exception handlers
*separate* rings guarantees that: if a DSI fires while normal code is half way
through writing a record, the handler writes to the other ring.
(This reasoning would need real memory barriers on a multicore CPU; it is a
stated assumption, not an accident.)

## Two APIs, because two environments

- `wdlog_write()` / `WDLOG_I(...)`: printf-style. Uses `vsnprintf`, which may
  take locks or allocate inside newlib. **Normal context only.**
- `wdlog_write_exc()`: takes a plain string, uses no libc. **Safe in exception
  handlers.** Structured data (addresses, registers) should travel in the event
  ring as binary, not be formatted into text here.

## Sinks, now and planned

| sink | when | safe from exceptions? | notes |
|------|------|-----------------------|-------|
| console (TV) | Phase 0 | no, main loop only | slow, but zero setup |
| in-band EVENT frames to the PC | Phase 1 | no, main loop only | needs the network up |
| USB Gecko (EXI serial) | Phase 4 | **yes**, talks to hardware directly, no IOS | the only way to see a crash that kills the network |
| SD-card file | later | no | survives a reboot |

## Compile-time stripping

`-DWDLOG_COMPILE_LEVEL=2` (INFO) removes DEBUG/TRACE calls from the binary
entirely: zero size, zero cost. Runtime filtering (per sink) is separate and
cheap: if no sink wants a level, the message is not even formatted.
