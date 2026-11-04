"""A WebSocket client small enough to not need a dependency.

Only what the AssemblyAI endpoints need: a TLS upgrade with custom headers,
masked client frames, and unmasked server frames reassembled across reads.
"""
import base64
import os
import socket
import ssl
import struct

TEXT, BINARY, CLOSE = 0x1, 0x2, 0x8


class WS:
    def __init__(self, host, path, headers=None, port=443, timeout=30):
        raw = socket.create_connection((host, port), timeout=timeout)
        self.sock = ssl.create_default_context().wrap_socket(
            raw, server_hostname=host)
        nonce = base64.b64encode(os.urandom(16)).decode()
        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {host}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {nonce}",
            "Sec-WebSocket-Version: 13",
        ]
        lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())

        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("server closed during handshake")
            buf += chunk
        head, self.buf = buf.split(b"\r\n\r\n", 1)
        status = head.decode(errors="replace").split("\r\n")[0]
        if "101" not in status:
            raise RuntimeError(f"handshake failed: {status}\n"
                               f"{head.decode(errors='replace')}")

    def send(self, opcode, payload):
        head = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head.append(0x80 | n)
        elif n < (1 << 16):
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        head += mask
        self.sock.sendall(bytes(head) +
                          bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send_text(self, s):
        self.send(TEXT, s.encode())

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv(self):
        """Returns (opcode, payload); raises ConnectionError when closed."""
        self._fill(2)
        b1, b2 = self.buf[0], self.buf[1]
        n, off = b2 & 0x7F, 2
        if n == 126:
            self._fill(4)
            n, off = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            self._fill(10)
            n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
        self._fill(off + n)
        payload = self.buf[off:off + n]
        self.buf = self.buf[off + n:]
        return b1 & 0x0F, payload

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
