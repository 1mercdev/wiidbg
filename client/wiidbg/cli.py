# SPDX-License-Identifier: GPL-3.0-only
# wiidbg - Wii debugger
# Copyright (C) 2026 1mercdev
"""Command-line front end:  python -m wiidbg HOST[:PORT] COMMAND ..."""
from __future__ import annotations

import argparse
import sys

from .conn import Connection, WdpError
from .messages import DEFAULT_PORT, Cap


def _split_host(arg: str) -> tuple[str, int]:
    host, sep, port = arg.rpartition(":")
    if sep and port.isdigit():
        return host, int(port)
    return arg, DEFAULT_PORT


def parse_int(text: str) -> int:
    """Accepts 123, 0x7b, 0b1111011. Used for addresses and lengths."""
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}") from None


def parse_hex_bytes(parts: list[str]) -> bytes:
    """'de ad be ef', 'deadbeef' and '0xde 0xad' all mean the same bytes."""
    text = "".join(p[2:] if p.lower().startswith("0x") else p for p in parts)
    try:
        return bytes.fromhex(text)
    except ValueError:
        raise ValueError(f"not valid hex bytes: {' '.join(parts)!r}") from None


def hexdump(data: bytes, base: int = 0) -> str:
    """Classic 16-bytes-per-line dump: address, hex, printable characters."""
    lines = []
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        hexpart = " ".join(f"{b:02x}" for b in row)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        lines.append(f"{base + off:08x}  {hexpart:<47}  |{text}|")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="wiidbg", description="Talk to a wiidbg stub on a Wii.")
    ap.add_argument("target", help=f"HOST or HOST:PORT (default port {DEFAULT_PORT})")
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("hello", help="handshake: show the stub's version and capabilities")

    p = sub.add_parser("ping", help="measure round-trip time")
    p.add_argument("-n", "--count", type=int, default=5, help="number of pings (default 5)")
    p.add_argument("-s", "--size", type=int, default=0, help="payload bytes per ping (default 0)")

    p = sub.add_parser("read", help="read memory and show a hex dump")
    p.add_argument("address", type=parse_int, help="start address, e.g. 0x80001000")
    p.add_argument("length", type=parse_int, help="number of bytes")
    p.add_argument("-o", "--output", help="write the raw bytes to this file instead of dumping")

    p = sub.add_parser("write", help="write bytes to memory")
    p.add_argument("address", type=parse_int, help="start address, e.g. 0x80001000")
    p.add_argument("hexbytes", nargs="*", help="bytes as hex, e.g. de ad be ef")
    p.add_argument("-f", "--file", help="write the contents of this file instead")
    return ap


def prepare(args: argparse.Namespace) -> None:
    """Check everything that can be checked without the network, before connecting.

    A typo in the arguments should fail at once, with a clear message, instead of
    after a connection attempt (and not depend on the stub being reachable).
    """
    if args.command == "write":
        if args.file:
            with open(args.file, "rb") as f:
                args.data = f.read()
        else:
            args.data = parse_hex_bytes(args.hexbytes)
        if not args.data:
            raise ValueError("nothing to write (give hex bytes or --file)")


def run(args: argparse.Namespace, conn: Connection) -> None:
    info = conn.hello()
    if args.command == "hello":
        caps = [c.name for c in Cap if info.capabilities & c] or ["none"]
        print(f"stub version : {info.stub_major}.{info.stub_minor}  (build {info.build!r})")
        print(f"capabilities : 0x{info.capabilities:08x}  ({', '.join(caps)})")
        print(f"max payload  : {info.max_payload} bytes")
    elif args.command == "ping":
        for i in range(args.count):
            ms = conn.ping_rtt(args.size) * 1000
            print(f"ping {i + 1}: {args.size} bytes, {ms:.2f} ms")
    elif args.command == "read":
        data = conn.read_memory(args.address, args.length)
        if args.output:
            with open(args.output, "wb") as f:
                f.write(data)
            print(f"wrote {len(data)} bytes to {args.output}")
        else:
            print(hexdump(data, args.address))
    elif args.command == "write":
        conn.write_memory(args.address, args.data)
        print(f"wrote {len(args.data)} bytes at 0x{args.address:08x}")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    host, port = _split_host(args.target)
    try:
        prepare(args)
        with Connection(host, port) as conn:
            run(args, conn)
    except (OSError, WdpError, ValueError) as e:
        print(f"wiidbg: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
