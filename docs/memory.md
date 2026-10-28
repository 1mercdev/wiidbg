# Memory model and caches

What you need to know to reason about `MEM_READ` / `MEM_WRITE` on a real Wii.
The protocol side is in [protocol.md](protocol.md); the code is
`common/wmem.[ch]` (policy) and `stub/source/wmem_wii.c` (the part that touches RAM).

## The memory map (what we allow)

| region | addresses (cached view) | size | notes |
|--------|-------------------------|-----:|-------|
| MEM1 | `0x80000000` – `0x817FFFFF` | 24 MiB | fast; most games keep code and hot data here |
| MEM2 | `0x90000000` – `0x93FFFFFF` | 64 MiB | bulk memory: textures, audio, big buffers |

The addresses programs use start with `0x8` or `0x9`. The Wii also exposes the
same RAM at `0xC...` / `0xD...` addresses (the *uncached* mirrors), and puts
hardware registers at `0xCC000000` and up. We allow **none** of those yet:

- reading some registers has side effects (it can acknowledge an interrupt or
  consume a byte from a FIFO), so a "harmless" read could disturb the console;
- an address that is not mapped at all raises a CPU exception, and the stub has no
  handler for it yet, so a bad request would crash the debugger itself.

The check is a single function, `wmem_range_ok()`, and nothing reads or writes
memory before it says yes.

## Why caches matter

The CPU does not talk to RAM directly. Between them sit small, fast memories:

```
         CPU ──► data cache ───────┐
          │                        ├──► RAM ◄── GPU, DMA, IOS coprocessor
          └────► instruction cache ┘     (these read RAM directly, NOT the CPU's caches)
```

- **Data cache.** When the CPU stores a value it usually lands in the cache first
  and reaches RAM *later*. Hardware that reads RAM directly can see the old value
  until the cache line is written back.
- **Instruction cache.** The CPU fetches code through its own cache, and **stores
  do not update it**. If you overwrite an instruction in RAM, the CPU can keep
  running the old copy it already cached.

Both caches work in **32-byte lines**. That is why the self-test counter is aligned
to 32 bytes: it sits alone in one cache line.

## What the stub does

- **Read:** a plain copy through the CPU's normal view. It returns exactly what the
  running program itself would see, including values still sitting in the data
  cache.
- **Write:** copy the bytes, then `DCFlushRange` (push modified cache lines out to
  RAM) and `ICInvalidateRange` (drop any stale cached copy as instructions). This
  is the standard "patch memory" sequence on the Wii. It is harmless for plain
  data and essential for code.

**Dolphin is lenient about all of this.** It does not model the caches faithfully,
so a missing flush or invalidate would pass every test in the emulator and fail on
a real console. That is why the cache calls are in the code even though nothing
here can prove they are right until it runs on hardware.

## Atomicity

One request is one step as far as the stub is concerned. A longer write is split
by the client into several requests, and **the sequence is not atomic**. If the
target is running, it can execute between two of them and see half a patch.
Patches that must be atomic have to fit one request (`max_payload - 4` bytes), or
wait for a "pause the target" command (planned for Phase 4).

## What reading memory cannot tell you

- Memory is not frozen while you read it: a running program may change it between
  two requests, so a large read is a series of snapshots, not one.
- A read through the cache shows the program's view. Data a device (GPU, DMA)
  wrote to RAM behind the CPU's back may not show until the program itself
  invalidates its cache, and the same is true for what you read.

## Testing it without a second program

The stub contains a **self-test area** (`stub/source/main.c`), since the stub and a
target program cannot yet run at the same time:

1. Start the stub (Dolphin or hardware); it shows the addresses of
   `counter` (a number the stub increments about 1000 times a second) and `scratch`
   (64 KiB of buffer).
2. `python3 -m wiidbg <ip> read <counter-address> 4` twice: the value changes.
3. `python3 -m wiidbg <ip> write <counter-address> 00 00 00 00`: the counter
   restarts from zero.
4. Write and read back a block bigger than one frame (see the README) to exercise
   the client's chunking on the real thing.

## Not yet

Uncached-mirror access, hardware registers, and watching a location for accesses
(Phase 3). Each of those needs its own safety reasoning.
