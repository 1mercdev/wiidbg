# SPDX-License-Identifier: GPL-3.0-only
"""Tests for wiidbg.frame, driven by the same vectors the C codec uses."""
import pathlib
import unittest

from wiidbg import frame

VECTORS = pathlib.Path(__file__).resolve().parents[2] / "tests" / "vectors.txt"

ERRORS = {
    "MAGIC": frame.BadMagic,
    "KIND": frame.BadKind,
    "FLAGS": frame.BadFlags,
    "TOO_BIG": frame.TooBig,
}


def load_vectors():
    valid, invalid = [], []
    for raw in VECTORS.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] == "valid":
            hexstr, *nums = parts[1:]
            valid.append((bytes.fromhex(hexstr), [int(n) for n in nums]))
        elif parts[0] == "invalid":
            invalid.append((bytes.fromhex(parts[1]), parts[2]))
        else:
            raise AssertionError(f"unknown vector type: {line}")
    return valid, invalid


class FrameVectors(unittest.TestCase):
    def test_vectors_were_loaded(self):
        valid, invalid = load_vectors()
        # Guards against a silently empty file making every test pass.
        self.assertGreaterEqual(len(valid), 6)
        self.assertGreaterEqual(len(invalid), 5)

    def test_valid_vectors_decode_and_round_trip(self):
        for wire, (ver, kind, seq, cmd, status, flags, length) in load_vectors()[0]:
            with self.subTest(wire=wire.hex()):
                h = frame.Header.decode(wire)
                self.assertEqual(
                    (h.version, h.kind, h.seq, h.cmd, h.status, h.flags, h.len),
                    (ver, kind, seq, cmd, status, flags, length),
                )
                self.assertEqual(h.encode(), wire)

    def test_invalid_vectors_are_rejected_with_the_right_error(self):
        for wire, name in load_vectors()[1]:
            with self.subTest(error=name, wire=wire.hex()):
                with self.assertRaises(ERRORS[name]):
                    frame.Header.decode(wire)

    def test_wrong_length_input(self):
        with self.assertRaises(frame.FrameError):
            frame.Header.decode(b"\x57\x44")

    def test_header_is_16_bytes(self):
        h = frame.Header(kind=frame.KIND_REQUEST, seq=1, cmd=2, len=0)
        self.assertEqual(len(h.encode()), frame.HEADER_SIZE)


if __name__ == "__main__":
    unittest.main()
