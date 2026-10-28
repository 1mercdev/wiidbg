# SPDX-License-Identifier: GPL-3.0-only
# wiidbg - Wii debugger
# Copyright (C) 2026 1mercdev
"""
Connection: a blocking WDP client over TCP.

It does three jobs the protocol needs on the PC side:
  1. FRAMING  - turn a byte stream into whole frames (recv_exact).
  2. PAIRING  - number each request (seq) and check the response matches it.
  3. ERRORS   - turn a non-zero status code into a Python exception.
"""
from __future__ import annotations

import socket
import struct
import time

from .frame import (HEADER_SIZE, KIND_REQUEST, KIND_RESPONSE, MAX_PAYLOAD,
                    VERSION, FrameError, Header, TooBig)
from .messages import DEFAULT_PORT, Cap, Cmd, Hello, Status


class WdpError(Exception):
    """Base class for everything this module raises on purpose."""


class ConnectionClosed(WdpError):
    """The stub closed the connection (or it dropped) mid-conversation."""


class ProtocolError(WdpError):
    """The stub sent something that violates the protocol."""


class StatusError(WdpError):
    """The stub answered a request with a non-zero status code."""

    def __init__(self, status: int, cmd: int, payload: bytes = b""):
        try:
            name = Status(status).name
        except ValueError:
            name = f"status {status}"
        super().__init__(f"command 0x{cmd:04x} failed: {name}")
        self.status = status
        self.cmd = cmd
        self.payload = payload


class PartialWriteError(WdpError):
    """A multi-chunk write failed part way through.

    Writes larger than one frame are split into several requests, and the
    sequence is NOT atomic: the first `written` bytes ARE in memory, the rest
    are not. A running program could have executed half-patched code in between.
    """

    def __init__(self, written: int, total: int, cause: StatusError):
        super().__init__(f"write failed after {written} of {total} bytes: {cause}")
        self.written = written
        self.total = total
        self.cause = cause


def _check_u32(name: str, value: int) -> None:
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError(f"{name} must fit in 32 bits, got {value}")


class Connection:
    def __init__(self, host: str, port: int = DEFAULT_PORT, timeout: float = 5.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._seq = 0
        self.hello_info: Hello | None = None

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- low level ---------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        """Read exactly n bytes. TCP may deliver them in any number of pieces."""
        chunks = []
        remaining = n
        while remaining:
            try:
                piece = self._sock.recv(remaining)
            except (ConnectionResetError, BrokenPipeError) as e:
                raise ConnectionClosed(str(e)) from e
            if not piece:
                raise ConnectionClosed("stub closed the connection")
            chunks.append(piece)
            remaining -= len(piece)
        return b"".join(chunks)

    def _next_seq(self) -> int:
        self._seq = (self._seq % 0xFFFF) + 1     # 1..65535, never 0
        return self._seq

    # -- the one primitive everything else is built on ---------------------
    def request(self, cmd: int, payload: bytes = b"") -> bytes:
        """Send one request, wait for its response, return the response payload."""
        if len(payload) > MAX_PAYLOAD:
            raise TooBig(f"payload of {len(payload)} bytes exceeds {MAX_PAYLOAD}")
        seq = self._next_seq()
        head = Header(kind=KIND_REQUEST, seq=seq, cmd=int(cmd), len=len(payload))
        try:
            self._sock.sendall(head.encode() + payload)
        except (ConnectionResetError, BrokenPipeError) as e:
            raise ConnectionClosed(str(e)) from e

        try:
            resp = Header.decode(self._recv_exact(HEADER_SIZE))
        except FrameError as e:
            raise ProtocolError(f"invalid response header: {e}") from e
        body = self._recv_exact(resp.len)

        if resp.version != VERSION:
            raise ProtocolError(f"stub speaks wire version {resp.version}, we speak {VERSION}")
        if resp.kind != KIND_RESPONSE:
            raise ProtocolError(f"expected a response frame, got kind {resp.kind}")
        if resp.seq != seq or resp.cmd != int(cmd):
            raise ProtocolError(
                f"response (seq {resp.seq}, cmd 0x{resp.cmd:04x}) does not match "
                f"request (seq {seq}, cmd 0x{int(cmd):04x})")
        if resp.status != Status.OK:
            raise StatusError(resp.status, int(cmd), body)
        return body

    # -- commands ----------------------------------------------------------
    def hello(self) -> Hello:
        """Handshake: learn the stub's version and what it can do."""
        self.hello_info = Hello.decode(self.request(Cmd.HELLO))
        return self.hello_info

    def ping(self, data: bytes = b"") -> bytes:
        """Round-trip `data` through the stub; raises if it comes back altered."""
        echoed = self.request(Cmd.PING, data)
        if echoed != data:
            raise ProtocolError("PING reply differs from what was sent")
        return echoed

    # -- memory ------------------------------------------------------------
    def supports(self, cap: Cap) -> bool:
        """True if the stub advertised this capability in HELLO."""
        if self.hello_info is None:
            self.hello()
        return bool(self.hello_info.capabilities & cap)

    def _max_payload(self) -> int:
        if self.hello_info is None:
            self.hello()
        return self.hello_info.max_payload

    def read_memory(self, addr: int, length: int) -> bytes:
        """Read `length` bytes starting at `addr`.

        Large reads are split into several requests, each at most the stub's
        max_payload bytes (learned from HELLO, never hard-coded).
        """
        _check_u32("addr", addr)
        _check_u32("length", length)
        if addr + length > 0x1_0000_0000:
            raise ValueError("range runs past the end of the 32-bit address space")
        chunk = self._max_payload()
        data = bytearray()
        while len(data) < length:
            n = min(chunk, length - len(data))
            part = self.request(Cmd.MEM_READ, struct.pack(">II", addr + len(data), n))
            if len(part) != n:
                raise ProtocolError(f"asked for {n} bytes, stub returned {len(part)}")
            data += part
        return bytes(data)

    def write_memory(self, addr: int, data: bytes) -> None:
        """Write `data` at `addr`.

        Each request carries 4 address bytes plus data, so a chunk holds at most
        max_payload - 4 bytes. A write that fits ONE chunk is applied in a single
        step; a longer one is several steps and can fail part way
        (PartialWriteError). Keep patches that must be atomic within one chunk.
        """
        data = bytes(data)
        _check_u32("addr", addr)
        if addr + len(data) > 0x1_0000_0000:
            raise ValueError("range runs past the end of the 32-bit address space")
        chunk = self._max_payload() - 4
        done = 0
        while done < len(data):
            part = data[done:done + chunk]
            try:
                self.request(Cmd.MEM_WRITE, struct.pack(">I", addr + done) + part)
            except StatusError as e:
                if done:
                    raise PartialWriteError(done, len(data), e) from e
                raise                       # nothing was written: the plain error is accurate
            done += len(part)

    def read_u32(self, addr: int) -> int:
        """Read one big-endian 32-bit value (the Wii's native byte order)."""
        return struct.unpack(">I", self.read_memory(addr, 4))[0]

    def write_u32(self, addr: int, value: int) -> None:
        _check_u32("value", value)
        self.write_memory(addr, struct.pack(">I", value))

    def ping_rtt(self, size: int = 0) -> float:
        """Seconds taken for one PING round trip with a payload of `size` bytes."""
        data = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
        start = time.perf_counter()
        self.ping(data)
        return time.perf_counter() - start
