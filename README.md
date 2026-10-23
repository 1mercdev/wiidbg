# wiidbg

An on-console **debugger and live-patching tool for the Nintendo Wii**, written
in C for the Wii and Python for the PC. It will let you read and write memory,
set data watchpoints and breakpoints, and inspect registers over Wi-Fi, from
your PC, while a homebrew program runs on the console.

It is also a **learning project**: the code, docs and decision records explain
*why*, not only *what*. If something is not explained, that is a bug in the docs.

> **Status: Phase 0 (scaffolding).** The stub boots and logs; the network,
> memory access and watchpoints come in the next phases. See the roadmap.

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
| `common/` | portable C shared by everything: `wdp` (frame codec), `wdlog` (logging core) | Wii **and** PC (tests) |
| `stub/` | the on-console agent, plus Wii-only log sinks | Wii |
| `target/` | a tiny program with a known counter, used as debugging ground truth | Wii |
| `client/` | Python package `wiidbg`: frame codec now, commands and CLI next | PC |
| `tests/` | `vectors.txt` (shared golden data) and the C host tests | PC |
| `docs/` | protocol spec, logging design, and `adr/` (decision records) | n/a |
| `tools/` | deploy helpers | PC |

Start reading here: `docs/protocol.md`, then `docs/logging.md`, then `docs/adr/`.

## Quick start: run the tests (no Wii needed)

```sh
make test      # C host tests (with sanitizers) + Python tests
```

This works anywhere with `gcc` and `python3`. The **same** `common/*.c` that
will run on the Wii is compiled for your PC and checked against
`tests/vectors.txt`, which the Python client is checked against too. That is how
we know both ends agree on the wire format before any hardware is involved.

## Building for the Wii

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) and the Wii
   packages: `sudo dkp-pacman -S wii-dev`.
2. In your shell: `export DEVKITPRO=/opt/devkitpro` and
   `export DEVKITPPC=$DEVKITPRO/devkitPPC`.
3. `make stub target` produces `stub/wiidbg-stub.dol` and
   `target/wiidbg-target.dol`.
4. Run a `.dol` in Dolphin (`make -C stub run`), or on a real Wii with the
   Homebrew Channel: `WIILOAD=tcp:<wii-ip> tools/deploy.sh stub`.

> The Wii-side code and Makefiles were written **without** access to devkitPPC,
> so they have not been compiled yet. Treat "does `make stub` build?" as the
> first thing to verify; the portable parts are already tested.

## Roadmap

| phase | goal | proves |
|------:|------|--------|
| 0 | scaffolding, spec, codecs, logging, tests | toolchain works; ends agree on the wire format |
| 1 | network transport, `HELLO`/`PING` | PC and Wii talk |
| 2 | memory read/write with range validation and cache maintenance | counter climbs, can be reset |
| 3 | data watchpoints (DABR) with single-step re-arm | hit PC matches the `stw` in the disassembly |
| 4 | execution breakpoints (IABR), register dump, resume/step | a real debugger |
| 5 | client REPL, disassembly, symbols, **GDB bridge** | gdb works |
| 6 | residency alongside other software | the hard research part |

## Security

WDP v1 has no authentication or encryption. A running stub lets **anyone on your
network** read and write the console's memory. Use it only on a network you
trust, and do not leave it running unattended.

## License

GPL-3.0-only. See `LICENSE` and `docs/adr/0004-license.md`.
