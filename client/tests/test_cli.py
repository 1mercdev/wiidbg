# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for the CLI helpers (no network involved)."""
import argparse
import contextlib
import io
import os
import tempfile
import unittest

from wiidbg.cli import build_parser, hexdump, main, parse_hex_bytes, parse_int


class ParseInt(unittest.TestCase):
    def test_accepts_decimal_hex_and_binary(self):
        self.assertEqual(parse_int("4096"), 4096)
        self.assertEqual(parse_int("0x80001000"), 0x80001000)
        self.assertEqual(parse_int("0b101"), 5)

    def test_rejects_garbage(self):
        for bad in ("", "xyz", "0xZZ", "1.5"):
            with self.assertRaises(argparse.ArgumentTypeError):
                parse_int(bad)


class ParseHexBytes(unittest.TestCase):
    def test_equivalent_spellings(self):
        want = b"\xde\xad\xbe\xef"
        self.assertEqual(parse_hex_bytes(["de", "ad", "be", "ef"]), want)
        self.assertEqual(parse_hex_bytes(["deadbeef"]), want)
        self.assertEqual(parse_hex_bytes(["0xde", "0xAD", "0xbe", "0xEF"]), want)

    def test_rejects_odd_length_and_non_hex(self):
        # A plain ValueError: this helper is called by our code, not by argparse.
        for bad in (["abc"], ["zz"], ["de", "a"]):
            with self.assertRaises(ValueError):
                parse_hex_bytes(bad)


class Hexdump(unittest.TestCase):
    def test_full_and_partial_lines(self):
        data = bytes(range(0x41, 0x41 + 20))          # "ABCDEFGHIJKLMNOPQRST"
        lines = hexdump(data, 0x80000000).splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("80000000  41 42 43"))
        self.assertTrue(lines[0].endswith("|ABCDEFGHIJKLMNOP|"))
        self.assertTrue(lines[1].startswith("80000010  51 52 53 54"))
        self.assertTrue(lines[1].endswith("|QRST|"))

    def test_columns_line_up_on_a_short_last_line(self):
        lines = hexdump(bytes(17)).splitlines()
        self.assertEqual(lines[0].index("|"), lines[1].index("|"))

    def test_unprintable_bytes_become_dots(self):
        self.assertTrue(hexdump(b"\x00\x1f\x7f\xff").endswith("|....|"))

    def test_empty(self):
        self.assertEqual(hexdump(b""), "")


class Parser(unittest.TestCase):
    def test_read_command(self):
        a = build_parser().parse_args(["h", "read", "0x80001000", "0x20"])
        self.assertEqual((a.command, a.address, a.length), ("read", 0x80001000, 0x20))

    def test_write_command(self):
        a = build_parser().parse_args(["h", "write", "0x90000000", "de", "ad"])
        self.assertEqual((a.address, a.hexbytes), (0x90000000, ["de", "ad"]))


class MainEntryPoint(unittest.TestCase):
    """Drives the real main(). Port 1 is unreachable: if an argument mistake were
    only noticed AFTER connecting, we would see 'Connection refused' instead of
    the argument message, so these tests also prove validation happens first."""

    def run_main(self, *argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = main(["127.0.0.1:1", *argv])
        return rc, err.getvalue()

    def test_bad_hex_is_a_clean_error_before_any_connection(self):
        rc, err = self.run_main("write", "0x80000000", "zz")
        self.assertEqual(rc, 1)
        self.assertIn("not valid hex", err)
        self.assertNotIn("refused", err.lower())
        self.assertNotIn("Traceback", err)

    def test_nothing_to_write_is_a_clean_error(self):
        rc, err = self.run_main("write", "0x80000000")
        self.assertEqual(rc, 1)
        self.assertIn("nothing to write", err)

    def test_missing_input_file_is_a_clean_error(self):
        rc, err = self.run_main("write", "0x80000000", "-f", "/nonexistent/patch.bin")
        self.assertEqual(rc, 1)
        self.assertIn("patch.bin", err)
        self.assertNotIn("refused", err.lower())

    def test_empty_input_file_is_refused(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            path = f.name
        self.addCleanup(os.unlink, path)
        rc, err = self.run_main("write", "0x80000000", "-f", path)
        self.assertEqual(rc, 1)
        self.assertIn("nothing to write", err)

    def test_valid_arguments_do_get_as_far_as_connecting(self):
        rc, err = self.run_main("write", "0x80000000", "de", "ad")
        self.assertEqual(rc, 1)
        self.assertIn("refused", err.lower())          # proves validation passed, then the network failed


if __name__ == "__main__":
    unittest.main()
