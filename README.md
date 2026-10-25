# wiidbg

An on-console **debugger and live-patching tool for the Nintendo Wii**, written
in C for the Wii and Python for the PC. It will let you read and write memory,
set data watchpoints and breakpoints, and inspect registers over Wi-Fi, from
your PC, while a homebrew program runs on the console.

It is also a **learning project**: the code, docs and decision records explain
*why*, not only *what*. If something is not explained, that is a bug in the docs.

> **Status: Phase 1 (network transport).** The stub brings up the network and
> answers `HELLO` and `PING`. Memory access and watchpoints come in the next
> phases. See the roadmap. Phase 1 is verified on a PC (unit, state-machine and
> end-to-end tests); the Wii-specific network code has not yet been run.

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
| `common/` | portable C shared by everything: `wdp` (codecs), `wdp_conn` (protocol state machine), `wdp_server` (connection loop), `wnet.h` (platform boundary), `wdlog` (logging core) | Wii **and** PC (tests) |
| `stub/` | the on-console agent, plus the Wii-only parts: libogc networking (`wnet_wii.c`) and log sinks | Wii |
| `target/` | a tiny program with a known counter, used as debugging ground truth | Wii |
| `client/` | Python package `wiidbg`: codecs, `Connection`, and a CLI | PC |
| `tests/` | `vectors.txt` (shared golden data), C host tests, and `hostserver` (a fake stub that runs on the PC) | PC |
| `docs/` | protocol spec, logging design, and `adr/` (decision records) | n/a |
| `tools/` | deploy helpers | PC |

Start reading here: `docs/architecture.md`, then `docs/protocol.md`, then `docs/logging.md`, then `docs/adr/`.

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

GPL-3.0-only. See `LICENSE`.
