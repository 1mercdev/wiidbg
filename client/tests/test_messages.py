# SPDX-License-Identifier: GPL-3.0-only
"""Tests for wiidbg.messages, driven by the HELLO vectors the C codec also reads."""
import pathlib
import unittest

from wiidbg import frame, messages

VECTORS = pathlib.Path(__file__).resolve().parents[2] / "tests" / "vectors.txt"

ERRORS = {"SHORT": messages.ShortPayload, "TOO_BIG": frame.TooBig}


def _payload(hexstr):
    return b"" if hexstr == "-" else bytes.fromhex(hexstr)


def load():
    good, bad = [], []
    for raw in VECTORS.read_text().splitlines():
        parts = raw.split()
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0] == "hello":
            hexstr, major, minor, caps, maxp, build = parts[1:7]
            good.append((_payload(hexstr), int(major), int(minor), int(caps),
                         int(maxp), "" if build == "-" else build))
        elif parts[0] == "hello_invalid":
            bad.append((_payload(parts[1]), parts[2]))
    return good, bad


class HelloVectors(unittest.TestCase):
    def test_vectors_were_loaded(self):
        good, bad = load()
        self.assertGreaterEqual(len(good), 4)
        self.assertGreaterEqual(len(bad), 4)

    def test_valid_hello_decodes_and_reencodes(self):
        for payload, major, minor, caps, maxp, build in load()[0]:
            with self.subTest(payload=payload.hex()):
                h = messages.Hello.decode(payload)
                self.assertEqual((h.stub_major, h.stub_minor, h.capabilities,
                                  h.max_payload, h.build),
                                 (major, minor, caps, maxp, build))
                # Re-encoding reproduces the LEADING bytes (trailing bytes are ignored).
                self.assertTrue(payload.startswith(h.encode()))

    def test_invalid_hello_is_rejected(self):
        for payload, name in load()[1]:
            with self.subTest(error=name, payload=payload.hex()):
                with self.assertRaises(ERRORS[name]):
                    messages.Hello.decode(payload)

    def test_encode_rejects_long_build(self):
        with self.assertRaises(frame.TooBig):
            messages.Hello(0, 1, 0, 0x4000, "x" * 32).encode()

    def test_capability_bits_match_c_header(self):
        self.assertEqual(int(messages.Cap.MEM_READ), 1)
        self.assertEqual(int(messages.Cap.MEM_WRITE), 2)
        self.assertEqual(int(messages.Cmd.MEM_READ), 0x0100)
        self.assertEqual(int(messages.Cmd.MEM_WRITE), 0x0101)

    def test_enums_match_c_header(self):
        # Spot-check values that must equal common/wdp.h.
        self.assertEqual(int(messages.Cmd.HELLO), 0x0001)
        self.assertEqual(int(messages.Cmd.PING), 0x0002)
        self.assertEqual(int(messages.Status.ERR_BAD_VERSION), 3)
        self.assertEqual(int(messages.Status.ERR_INTERNAL), 8)


if __name__ == "__main__":
    unittest.main()
