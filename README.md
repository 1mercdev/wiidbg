# wiidbg

An on-console **debugger and live-patching tool for the Nintendo Wii**, written
in C for the Wii and Python for the PC. It will let you read and write memory,
set data watchpoints and breakpoints, and inspect registers over Wi-Fi, from
your PC, while a homebrew program runs on the console.

It is also a **learning project**: the code, docs and decision records explain
*why*, not only *what*. If something is not explained, that is a bug in the docs.

> **Status: Phase 2 (memory access).** The stub answers `HELLO`, `PING`,
> `MEM_READ` and `MEM_WRITE`: from your PC you can read and write the console's RAM.
> Watchpoints and breakpoints come in the next phases. See the roadmap. Phase 1 was
> confirmed in Dolphin; Phase 2 is verified on a PC (unit, state-machine and
> end-to-end tests) and still to be confirmed in Dolphin and on real hardware.

## How it fits together

```
        PC                                          Wii
┌──────────────────┐   WDP frames over TCP   ┌─────────────────────────┐
│ client (Python)  │ ◄─────────────────────► │ stub (C, devkitPPC)     │
│  - frame codec   │  16-byte header + data  │  - network loop         │
│  - commands, CLI │                         │  - command dispatch     │
│  - (later) GDB   │                         │  - memory / DABR / IABR │
│    bridge        │                         │  - exception handlers   │
└──────────────────┘                         └───────────▲─────────────┘
                                                         │ debugs
                                             ┌───────────┴─────────────┐
                                             │ target (test program)   │
                                             └─────────────────────────┘
```

## Repository layout

| path | what | runs on |
|------|------|---------|
| `common/` | portable C shared by everything: `wdp` (codecs), `wdp_conn` (protocol state machine), `wdp_server` (connection loop), `wnet.h` (network boundary), `wmem` (memory access policy and backend interface), `wdlog` (logging core) | Wii **and** PC (tests) |
| `stub/` | the on-console agent, plus the Wii-only parts: libogc networking (`wnet_wii.c`), real-RAM access (`wmem_wii.c`) and log sinks | Wii |
| `target/` | a tiny program with a known counter, used as debugging ground truth | Wii |
| `client/` | Python package `wiidbg`: codecs, `Connection`, and a CLI | PC |
| `tests/` | `vectors.txt` (shared golden data), C host tests, and `hostserver` (a fake stub that runs on the PC) | PC |
| `docs/` | protocol spec, memory and cache notes, logging design, and `adr/` (decision records) | n/a |
| `tools/` | deploy helpers | PC |

Start reading here: `docs/architecture.md`, then `docs/protocol.md`, then `docs/memory.md`, then `docs/logging.md`, then `docs/adr/`.

## Quick start: run the tests (no Wii needed)

```sh
make test      # C host tests (with sanitizers) + Python tests
```

This works anywhere with `gcc` and `python3`. The **same** `common/*.c` that
runs on the Wii is compiled for your PC and checked against
`tests/vectors.txt`, which the Python client is checked against too. It also
starts `hostserver` (the real server loop on PC sockets) and talks to it with the
real Python client. That is how we know both ends agree before any hardware is
involved.

## Try it without a Wii

```sh
make hostserver
tests/host/bin/hostserver              # prints: listening 127.0.0.1:<port>

cd client                              # in a second terminal
python3 -m wiidbg 127.0.0.1:<port> hello
python3 -m wiidbg 127.0.0.1:<port> ping -n 5 -s 4096
```

## Try it in Dolphin (or on a Wii)

```sh
make stub
make -C stub run  # or run the .dol on a real Wii
```

The stub shows its IP address on screen and a ready-to-copy command, e.g.
`python -m wiidbg <ip> hello`. In Dolphin the address shown may not be reachable
from your PC the way a real console's would be; if it is not, try `127.0.0.1`.
(Dolphin's network emulation is the first thing to verify in Phase 1.)

## Reading and writing memory

Works the same against `hostserver` (fake RAM), Dolphin, or a Wii. From `client/`:

```sh
# read 32 bytes: a hex dump with addresses and printable characters
python3 -m wiidbg <host>:<port> read 0x80001000 32

# write bytes (spaces and 0x prefixes are optional)
python3 -m wiidbg <host>:<port> write 0x80001000 de ad be ef

# larger transfers are split into frames automatically
python3 -m wiidbg <host>:<port> read 0x90000000 0x10000 -o dump.bin
python3 -m wiidbg <host>:<port> write 0x90000000 --file patch.bin
```

Only the two RAM areas are allowed (`0x80000000-0x817FFFFF` and
`0x90000000-0x93FFFFFF`); anything else is refused with `ERR_RANGE` and nothing is
touched. From Python: `conn.read_memory(addr, n)`, `conn.write_memory(addr, data)`,
`conn.read_u32(addr)`, `conn.write_u32(addr, value)`.

**Proving it in Dolphin.** The stub carries a self-test area. It prints the
address of a `counter` it increments about 1000 times a second, and of a 64 KiB
`scratch` buffer. Read the counter twice and watch it change; write zeros to it and
watch it restart. See `docs/memory.md` for the full walk-through and for what
Dolphin cannot prove about caches.

## Building for the Wii

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) and the Wii
   packages: `sudo dkp-pacman -S wii-dev`.
2. In your shell: `export DEVKITPRO=/opt/devkitpro` and
   `export DEVKITPPC=$DEVKITPRO/devkitPPC`.
3. `make stub target` produces `stub/wiidbg-stub.dol` and
   `target/wiidbg-target.dol`.
4. Run a `.dol` in Dolphin (`make -C stub run`), or on a real Wii with the
   Homebrew Channel: `WIILOAD=tcp:<wii-ip> tools/deploy.sh stub`.

## Roadmap

| phase | goal | proves |
|------:|------|--------|
| 0 ✅ | scaffolding, spec, codecs, logging, tests | toolchain works; ends agree on the wire format |
| 1 ✅ | network transport, `HELLO`/`PING` | PC and Wii talk. Still to confirm in Dolphin / on hardware |
| 2 ✅ |(needs test on wii) memory read/write with range validation and cache maintenance | counter climbs, can be reset |
| 3 | data watchpoints (DABR) with single-step re-arm | hit PC matches the `stw` in the disassembly |
| 4 | execution breakpoints (IABR), register dump, resume/step | a real debugger |
| 5 | client REPL, disassembly, symbols, **GDB bridge** | gdb works |
| 6 | residency alongside other software | the hard research part |

## Security

WDP v1 has no authentication or encryption. Since Phase 2, a running stub lets
**anyone on your network read and write the console's memory**, which is the same
as running code on it. Use it only on a network you trust, and do not leave it
running unattended. Authentication should be settled before any public release.

## License

GPL-3.0-only. See `LICENSE`.
