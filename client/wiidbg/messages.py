# SPDX-License-Identifier: GPL-3.0-only
# wiidbg - Wii debugger
# Copyright (C) 2026 1mercdev
"""
WDP message layer: command ids, status codes and payload codecs.

Python twin of the constants in common/wdp.h and of wdp_hello_encode/decode
in common/wdp.c. Specification: docs/protocol.md.
"""
from __future__ import annotations

import enum
import struct
from dataclasses import dataclass

from .frame import FrameError, TooBig

BUILD_MAX = 32            # includes the C terminator, so strings are <= 31 bytes
DEFAULT_PORT = 4242
HELLO_FIXED_SIZE = 14
_HELLO_FIXED = struct.Struct(">BBHIIH")   # major minor reserved caps max_payload build_len
assert _HELLO_FIXED.size == HELLO_FIXED_SIZE


class Cmd(enum.IntEnum):
    HELLO = 0x0001
    PING = 0x0002


class Status(enum.IntEnum):
    OK = 0
    ERR_UNKNOWN_CMD = 1
    ERR_BAD_PAYLOAD = 2
    ERR_BAD_VERSION = 3
    ERR_RANGE = 4
    ERR_TOO_BIG = 5
    ERR_BUSY = 6
    ERR_UNSUPPORTED = 7
    ERR_INTERNAL = 8


class ShortPayload(FrameError):
    """A payload is shorter than the fields it must contain."""


@dataclass(frozen=True)
class Hello:
    stub_major: int
    stub_minor: int
    capabilities: int
    max_payload: int
    build: str

    def encode(self) -> bytes:
        raw = self.build.encode("utf-8")
        if len(raw) >= BUILD_MAX:
            raise TooBig(f"build string longer than {BUILD_MAX - 1} bytes")
        return _HELLO_FIXED.pack(self.stub_major, self.stub_minor, 0,
                                 self.capabilities, self.max_payload,
                                 len(raw)) + raw

    @classmethod
    def decode(cls, data: bytes) -> "Hello":
        if len(data) < HELLO_FIXED_SIZE:
            raise ShortPayload(f"HELLO payload needs {HELLO_FIXED_SIZE} bytes, got {len(data)}")
        major, minor, _reserved, caps, max_payload, n = _HELLO_FIXED.unpack_from(data)
        if n >= BUILD_MAX:
            raise TooBig(f"build_len {n} exceeds {BUILD_MAX - 1}")
        if len(data) < HELLO_FIXED_SIZE + n:
            raise ShortPayload("HELLO build string is truncated")
        # Bytes after the build string are ignored on purpose (forward compat).
        build = data[HELLO_FIXED_SIZE:HELLO_FIXED_SIZE + n].decode("utf-8")
        return cls(major, minor, caps, max_payload, build)
