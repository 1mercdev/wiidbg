# SPDX-License-Identifier: GPL-3.0-only
# wiidbg - Wii debugger
# Copyright (C) 2026 1mercdev
"""Command-line front end:  python -m wiidbg HOST[:PORT] {hello|ping}"""
from __future__ import annotations

import argparse
import sys

from .conn import Connection, WdpError
from .messages import DEFAULT_PORT


def _split_host(arg: str) -> tuple[str, int]:
    host, sep, port = arg.rpartition(":")
    if sep and port.isdigit():
        return host, int(port)
    return arg, DEFAULT_PORT


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="wiidbg", description="Talk to a wiidbg stub on a Wii.")
    ap.add_argument("target", help=f"HOST or HOST:PORT (default port {DEFAULT_PORT})")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("hello", help="handshake: show the stub's version and capabilities")
    p = sub.add_parser("ping", help="measure round-trip time")
    p.add_argument("-n", "--count", type=int, default=5, help="number of pings (default 5)")
    p.add_argument("-s", "--size", type=int, default=0, help="payload bytes per ping (default 0)")
    args = ap.parse_args(argv)

    host, port = _split_host(args.target)
    try:
        with Connection(host, port) as conn:
            info = conn.hello()
            if args.command == "hello":
                print(f"stub version : {info.stub_major}.{info.stub_minor}  (build {info.build!r})")
                print(f"capabilities : 0x{info.capabilities:08x}")
                print(f"max payload  : {info.max_payload} bytes")
            else:
                for i in range(args.count):
                    ms = conn.ping_rtt(args.size) * 1000
                    print(f"ping {i + 1}: {args.size} bytes, {ms:.2f} ms")
    except (OSError, WdpError) as e:
        print(f"wiidbg: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
