# SPDX-License-Identifier: GPL-3.0-only
"""
End-to-end tests: the real Python client against the real C server loop.

The server is tests/host/bin/hostserver (built by `make test-c`): the exact
same wdp_server/wdp_conn/wdp code as the Wii stub, with PC sockets underneath.
If the binary has not been built the tests are skipped, not failed.
"""
import pathlib
import socket
import subprocess
import time
import unittest

from wiidbg import frame
from wiidbg.conn import (Connection, ConnectionClosed, PartialWriteError,
                         ProtocolError, StatusError)
from wiidbg.messages import (MEM1_BASE, MEM1_SIZE, MEM2_BASE, MEM2_SIZE, Cap, Cmd,
                             Status)

HOSTSERVER = pathlib.Path(__file__).resolve().parents[2] / "tests" / "host" / "bin" / "hostserver"


@unittest.skipUnless(HOSTSERVER.exists(), "build it first: make test-c")
class IntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = subprocess.Popen([str(HOSTSERVER)], stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True)
        line = cls.proc.stdout.readline()          # "listening 127.0.0.1:<port>"
        if not line.startswith("listening"):
            raise RuntimeError(f"hostserver did not start: {line!r}")
        cls.port = int(line.rsplit(":", 1)[1])

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)

    # -- helpers -----------------------------------------------------------
    def client(self):
        return Connection("127.0.0.1", self.port, timeout=5)

    def raw(self):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.addCleanup(s.close)
        return s

    @staticmethod
    def read_all_until_eof(s):
        data = b""
        while True:
            piece = s.recv(4096)
            if not piece:
                return data
            data += piece

    # -- normal operation --------------------------------------------------
    def test_hello(self):
        with self.client() as c:
            h = c.hello()
        self.assertEqual((h.stub_major, h.stub_minor), (0, 2))
        self.assertEqual(h.max_payload, frame.MAX_PAYLOAD)
        self.assertEqual(h.capabilities, Cap.MEM_READ | Cap.MEM_WRITE)
        self.assertTrue(h.build.startswith("0.2.0"))

    def test_ping_sizes(self):
        with self.client() as c:
            for size in (0, 1, 15, 16, 17, 1000, frame.MAX_PAYLOAD):
                data = bytes((i * 31 + size) & 0xFF for i in range(size))
                self.assertEqual(c.ping(data), data, f"size {size}")

    def test_many_requests_one_connection(self):
        with self.client() as c:
            for i in range(300):                  # also exercises seq numbering
                c.ping(bytes([i & 0xFF]) * (i % 40))

    def test_unknown_command_is_an_error_not_a_disconnect(self):
        with self.client() as c:
            with self.assertRaises(StatusError) as cm:
                c.request(0x7777)
            self.assertEqual(cm.exception.status, Status.ERR_UNKNOWN_CMD)
            c.ping(b"still alive")                # same connection keeps working

    def test_hello_with_payload_is_rejected(self):
        with self.client() as c:
            with self.assertRaises(StatusError) as cm:
                c.request(Cmd.HELLO, b"x")
            self.assertEqual(cm.exception.status, Status.ERR_BAD_PAYLOAD)

    def test_client_refuses_oversize_payload_locally(self):
        with self.client() as c:
            with self.assertRaises(frame.TooBig):
                c.request(Cmd.PING, bytes(frame.MAX_PAYLOAD + 1))

    # -- awkward but legal traffic -----------------------------------------
    def test_request_sent_one_byte_at_a_time(self):
        s = self.raw()
        req = frame.Header(kind=frame.KIND_REQUEST, seq=1, cmd=Cmd.PING, len=5).encode() + b"slowx"
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for b in req:
            s.sendall(bytes([b]))
            time.sleep(0.002)
        resp = s.recv(64)
        while len(resp) < frame.HEADER_SIZE + 5:
            resp += s.recv(64)
        self.assertEqual(resp[frame.HEADER_SIZE:], b"slowx")

    def test_pipelined_requests_get_ordered_replies(self):
        s = self.raw()
        blob = b""
        for seq in range(1, 6):
            payload = bytes([seq]) * (seq * 100)
            blob += frame.Header(kind=frame.KIND_REQUEST, seq=seq, cmd=Cmd.PING,
                                 len=len(payload)).encode() + payload
        s.sendall(blob)                           # all five in one go
        for seq in range(1, 6):
            head = b""
            while len(head) < frame.HEADER_SIZE:
                head += s.recv(frame.HEADER_SIZE - len(head))
            h = frame.Header.decode(head)
            body = b""
            while len(body) < h.len:
                body += s.recv(h.len - len(body))
            self.assertEqual((h.seq, h.status, h.len), (seq, 0, seq * 100))
            self.assertEqual(body, bytes([seq]) * (seq * 100))

    # -- hostile or broken clients -----------------------------------------
    def test_non_wdp_traffic_is_dropped_silently(self):
        s = self.raw()
        s.sendall(b"GET / HTTP/1.1\r\nHost: wii\r\n\r\n")
        self.assertEqual(self.read_all_until_eof(s), b"")     # closed, no reply

    def test_huge_length_is_dropped_before_any_payload_is_read(self):
        s = self.raw()
        bad = frame.Header(kind=frame.KIND_REQUEST, seq=1, cmd=Cmd.PING, len=0).encode()
        bad = bad[:12] + b"\xff\xff\xff\xff"                  # claims a 4 GiB payload
        s.sendall(bad)
        self.assertEqual(self.read_all_until_eof(s), b"")

    def test_wrong_version_gets_an_answer_then_a_hangup(self):
        s = self.raw()
        req = bytearray(frame.Header(kind=frame.KIND_REQUEST, seq=42, cmd=Cmd.HELLO).encode())
        req[2] = 9                                            # version we do not speak
        s.sendall(bytes(req))
        data = self.read_all_until_eof(s)
        h = frame.Header.decode(data[:frame.HEADER_SIZE])
        self.assertEqual((h.seq, h.status), (42, Status.ERR_BAD_VERSION))
        self.assertEqual(data[frame.HEADER_SIZE:], bytes([frame.VERSION]))

    def test_server_survives_abusive_clients(self):
        for junk in (b"", b"\x00" * 16, b"WD", bytes(range(256)) * 4):
            s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            s.sendall(junk)
            s.close()
        with self.client() as c:                              # still healthy afterwards
            c.ping(b"ok")


    # -- memory (Phase 2) --------------------------------------------------
    # The server's RAM is fake (plain buffers), so these test the PROTOCOL:
    # what you write is what you read back, across chunk and region boundaries.
    # Most tests use their own address range. The tests that touch the very end of
    # a region cannot, so they never assume what was there before.

    def test_seed_text_is_readable(self):
        with self.client() as c:
            self.assertEqual(c.read_memory(0x80000100, 20), b"wiidbg host fake RAM")

    def test_write_then_read_round_trip(self):
        with self.client() as c:
            data = bytes(range(256))
            c.write_memory(0x80010000, data)
            self.assertEqual(c.read_memory(0x80010000, 256), data)
            self.assertEqual(c.read_memory(0x80010010, 16), data[16:32])   # a slice of it

    def test_unaligned_addresses_and_odd_sizes(self):
        with self.client() as c:
            for i, size in enumerate((1, 2, 3, 5, 7, 31, 33)):
                addr = 0x80020001 + i * 100                                 # never 4-aligned
                data = bytes((j * 7 + size) & 0xFF for j in range(size))
                c.write_memory(addr, data)
                self.assertEqual(c.read_memory(addr, size), data, f"size {size}")

    def test_mem2_works_too(self):
        with self.client() as c:
            c.write_memory(MEM2_BASE + 0x1234, b"in MEM2")
            self.assertEqual(c.read_memory(MEM2_BASE + 0x1234, 7), b"in MEM2")

    def test_large_transfers_are_chunked_by_the_client(self):
        with self.client() as c:
            size = 100_000                         # about 6 chunks each way
            data = bytes((i * 31 + (i >> 8)) & 0xFF for i in range(size))
            c.write_memory(0x80100001, data)
            self.assertEqual(c.read_memory(0x80100001, size), data)

    def test_chunk_boundaries_are_exact(self):
        with self.client() as c:
            chunk_w = c.hello().max_payload - 4    # biggest write per request
            for size in (chunk_w - 1, chunk_w, chunk_w + 1, 2 * chunk_w, 2 * chunk_w + 1):
                data = bytes((i + size) & 0xFF for i in range(size))
                c.write_memory(0x80200000, data)
                self.assertEqual(c.read_memory(0x80200000, size), data, f"size {size}")

    def test_regions_edges(self):
        with self.client() as c:
            last = MEM1_BASE + MEM1_SIZE - 1
            c.write_memory(last, b"Z")                                     # the very last byte
            self.assertEqual(c.read_memory(last, 1), b"Z")
            with self.assertRaises(StatusError) as cm:                     # one past it
                c.read_memory(last + 1, 1)
            self.assertEqual(cm.exception.status, Status.ERR_RANGE)
            end2 = MEM2_BASE + MEM2_SIZE - 1
            c.write_memory(end2, b"Y")
            self.assertEqual(c.read_memory(end2, 1), b"Y")

    def test_forbidden_addresses_are_refused_and_the_connection_survives(self):
        with self.client() as c:
            for addr in (0x00000000, 0x0C000000, 0xC0000000, 0xCC000000, 0xFFFFFFFC):
                with self.assertRaises(StatusError) as cm:
                    c.read_memory(addr, 4)
                self.assertEqual(cm.exception.status, Status.ERR_RANGE, hex(addr))
                with self.assertRaises(StatusError) as cm:
                    c.write_memory(addr, b"\x00\x00\x00\x00")
                self.assertEqual(cm.exception.status, Status.ERR_RANGE, hex(addr))
            c.ping(b"still here")                                          # no disconnect

    def test_a_refused_write_changes_nothing(self):
        with self.client() as c:
            end = MEM1_BASE + MEM1_SIZE
            c.write_memory(end - 4, b"\xAA\xBB\xCC\xDD")
            with self.assertRaises(StatusError):                          # 8 bytes: 4 inside, 4 outside
                c.write_memory(end - 4, b"\x11" * 8)
            self.assertEqual(c.read_memory(end - 4, 4), b"\xAA\xBB\xCC\xDD")   # untouched

    def test_partial_write_is_reported_honestly(self):
        with self.client() as c:
            chunk = c.hello().max_payload - 4
            end = MEM1_BASE + MEM1_SIZE
            start = end - chunk - 10               # room for chunk 1, not for chunk 2
            # Other tests also touch the end of MEM1, so do not assume zeros:
            # remember what is there and check it is unchanged afterwards.
            tail_before = c.read_memory(end - 10, 10)
            with self.assertRaises(PartialWriteError) as cm:
                c.write_memory(start, b"\x77" * (chunk + 100))
            e = cm.exception
            self.assertEqual((e.written, e.total), (chunk, chunk + 100))
            self.assertEqual(e.cause.status, Status.ERR_RANGE)
            self.assertEqual(c.read_memory(start, 4), b"\x77" * 4)         # first chunk DID land
            self.assertEqual(c.read_memory(end - 10, 10), tail_before)     # second did not

    def test_u32_helpers_use_big_endian(self):
        with self.client() as c:
            c.write_u32(0x80030000, 0x01020304)
            self.assertEqual(c.read_memory(0x80030000, 4), b"\x01\x02\x03\x04")
            self.assertEqual(c.read_u32(0x80030000), 0x01020304)
            c.write_u32(0x80030004, 0xFFFFFFFF)
            self.assertEqual(c.read_u32(0x80030004), 0xFFFFFFFF)

    def test_empty_transfers_need_no_network(self):
        with self.client() as c:
            self.assertEqual(c.read_memory(0x80000000, 0), b"")
            c.write_memory(0x80000000, b"")

    def test_argument_validation_happens_before_sending(self):
        with self.client() as c:
            for bad in (lambda: c.read_memory(-1, 4), lambda: c.read_memory(0, -1),
                        lambda: c.read_memory(0x1_0000_0000, 1),
                        lambda: c.read_memory(0xFFFFFFFF, 2),
                        lambda: c.write_memory(0xFFFFFFFF, b"ab"),
                        lambda: c.write_u32(0x80000000, 0x1_0000_0000)):
                with self.assertRaises(ValueError):
                    bad()
            c.ping(b"nothing was sent for those")

    def test_malformed_memory_requests_get_bad_payload(self):
        with self.client() as c:
            for payload in (b"", b"\x00" * 7, b"\x00" * 9):
                with self.assertRaises(StatusError) as cm:
                    c.request(Cmd.MEM_READ, payload)
                self.assertEqual(cm.exception.status, Status.ERR_BAD_PAYLOAD)
            for payload in (b"", b"\x80\x00\x00\x00"):                       # no data bytes
                with self.assertRaises(StatusError) as cm:
                    c.request(Cmd.MEM_WRITE, payload)
                self.assertEqual(cm.exception.status, Status.ERR_BAD_PAYLOAD)

    def test_oversize_read_request_is_refused_cleanly(self):
        with self.client() as c:
            import struct
            with self.assertRaises(StatusError) as cm:
                c.request(Cmd.MEM_READ, struct.pack(">II", 0x80000000, frame.MAX_PAYLOAD + 1))
            self.assertEqual(cm.exception.status, Status.ERR_TOO_BIG)
            c.ping(b"ok")

    def test_supports_reports_capabilities(self):
        with self.client() as c:
            self.assertTrue(c.supports(Cap.MEM_READ))
            self.assertTrue(c.supports(Cap.MEM_WRITE))

    def test_two_sessions_see_the_same_memory(self):
        a = self.client()
        a.write_memory(0x80040000, b"shared")
        a.close()
        with self.client() as b:
            self.assertEqual(b.read_memory(0x80040000, 6), b"shared")

    # -- session policy ----------------------------------------------------
    def test_newest_connection_replaces_the_old_one(self):
        a = self.client()
        a.hello()
        with self.client() as b:
            b.hello()                                         # B is served...
            # A must see an explicit close. A timeout would NOT count: that
            # would mean the server left A hanging instead of dropping it.
            with self.assertRaises(ConnectionClosed):
                a.ping(b"hello?")
        a.close()

    def test_reconnect_after_disconnect(self):
        for _ in range(3):
            with self.client() as c:
                c.ping(b"again")


if __name__ == "__main__":
    unittest.main()
