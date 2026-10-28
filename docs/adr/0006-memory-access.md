# ADR 0006: Memory access: one central range policy, an injected backend, chunking in the client

Status: accepted

## Context

Phase 2 lets a client read and write the console's memory. That is the first
feature that can crash the console (an unmapped address raises a CPU exception the
stub cannot handle yet) and the first that can change what a running program
does. The protocol must make the safe path the only path, stay testable without
hardware, and not hard-wire limits that will be painful to lift later.

## Decisions

1. **One central range policy** (`wmem_range_ok`, `common/wmem.c`). Allowed: the
   cached MEM1 and MEM2 ranges only. The protocol layer calls it before touching
   any backend. The backend is told it may assume the range is valid and does not
   check again, so two checks can never disagree.
2. **The backend is injected** (`wmem_ops`, passed in the connection's config) the
   same way the network is (ADR 0005). The Wii backend copies memory and does the
   cache maintenance; tests inject recording fakes that let us assert that a
   refused request never reached memory.
3. **Large transfers are split by the client**, using the `max_payload` the stub
   reports in HELLO. The stub stays simple: one request, one step, no multi-frame
   state. Address and length fields are 32 bits, so a future stub can raise the
   limit without a wire change.
4. **Writes are copy + flush + invalidate.** Reads are a plain copy through the
   cache (the program's own view).
5. **Capabilities are derived from the backend**, not configured by hand, so HELLO
   cannot advertise something the stub lacks.
6. **A zero-length read is `ERR_BAD_PAYLOAD`**, not an empty success: it is almost
   certainly a client bug, and rejecting it keeps the rules uniform (every request
   names at least one byte).

## Consequences

- (+) A bad address cannot reach memory: it is rejected in one place that is tested
  at every boundary, including 32-bit wraparound.
- (+) Almost everything is verified on a PC, including "a refused write changes
  nothing" and "a client request is handled the same however TCP splits it".
- (+) The stub has no multi-frame transfer state, which matters later when code
  must also run in exception context.
- (−) Chunked writes are **not atomic**. Documented in protocol.md and memory.md and
  surfaced to users as `PartialWriteError`. The real fix (pausing the target)
  arrives with execution control in Phase 4.
- (−) The stub can overwrite its own memory (nothing protects it). A debugger has to
  be able to write anywhere in RAM; protecting itself would need a reserved-range
  list. Revisit if it bites.
- (−) The cache maintenance is correct by construction and by documentation, but
  cannot be verified in Dolphin. It must be re-checked on hardware.
- (−) No authentication: with this phase an open port means anyone on the network
  can write to the console. Authentication should be settled before any public
  release.
