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
from wiidbg.conn import Connection, ConnectionClosed, ProtocolError, StatusError
from wiidbg.messages import Cmd, Status

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
        self.assertEqual((h.stub_major, h.stub_minor), (0, 1))
        self.assertEqual(h.max_payload, frame.MAX_PAYLOAD)
        self.assertEqual(h.capabilities, 0)
        self.assertTrue(h.build.startswith("0.1.0"))

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
