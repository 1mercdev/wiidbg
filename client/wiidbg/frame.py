# SPDX-License-Identifier: GPL-3.0-only
# wiidbg - Wii debugger
# Copyright (C) 2026 1mercdev
"""
WDP frame layer: the Python twin of common/wdp.[ch].

The wire format is specified in docs/protocol.md. This module and the C
codec must agree byte for byte; tests/vectors.txt is the shared proof.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x5744            # ASCII "WD"
VERSION = 1
HEADER_SIZE = 16
MAX_PAYLOAD = 0x4000      # keep equal to WDP_MAX_PAYLOAD in common/wdp.h

# One struct format string IS the layout spec:
#   >  big-endian, no padding
#   H  magic   B version   B kind   H seq   H cmd   H status   H flags   I len
_HEADER = struct.Struct(">HBBHHHHI")
assert _HEADER.size == HEADER_SIZE

KIND_REQUEST, KIND_RESPONSE, KIND_EVENT = 0, 1, 2
_KIND_COUNT = 3


class FrameError(Exception):
    """Base class: the bytes on the wire are not a valid WDP frame."""


class BadMagic(FrameError):
    pass


class BadKind(FrameError):
    pass


class BadFlags(FrameError):
    pass


class TooBig(FrameError):
    pass


@dataclass(frozen=True)
class Header:
    kind: int
    seq: int
    cmd: int
    status: int = 0
    len: int = 0
    version: int = VERSION
    flags: int = 0
    magic: int = MAGIC

    def encode(self) -> bytes:
        return _HEADER.pack(self.magic, self.version, self.kind, self.seq,
                            self.cmd, self.status, self.flags, self.len)

    @classmethod
    def decode(cls, data: bytes) -> "Header":
        if len(data) != HEADER_SIZE:
            raise FrameError(f"header must be {HEADER_SIZE} bytes, got {len(data)}")
        magic, version, kind, seq, cmd, status, flags, length = _HEADER.unpack(data)
        # Same validation order as wdp_header_decode() in C.
        if magic != MAGIC:
            raise BadMagic(f"bad magic 0x{magic:04x}")
        if kind >= _KIND_COUNT:
            raise BadKind(f"unknown kind {kind}")
        if flags != 0:
            raise BadFlags(f"unsupported flags 0x{flags:04x}")
        if length > MAX_PAYLOAD:
            raise TooBig(f"payload length {length} exceeds {MAX_PAYLOAD}")
        return cls(kind=kind, seq=seq, cmd=cmd, status=status, len=length,
                   version=version, flags=flags, magic=magic)
