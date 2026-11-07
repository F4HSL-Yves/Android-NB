#!/usr/bin/env python3
"""Checks a running nbd WebSocket from a PC, standard library only.

    python3 nbws_probe.py ws://192.168.2.1:8073 [--freq 10489900000] [--seconds 10]

Prints the hello and status messages, sends the optional commands and counts
the Opus audio packets (and the bitrate actually received).
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
from urllib.parse import urlparse


def send_frame(sock, opcode, payload):
    mask = os.urandom(4)
    head = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed by nbd")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def frame(self):
        b0, b1 = self.need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.need(8))[0]
        return b0 & 0x0F, self.need(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url", nargs="?", default="ws://127.0.0.1:8073")
    ap.add_argument("--freq", type=float, help="USB dial frequency, Hz")
    ap.add_argument("--filter", help="low,high in Hz, e.g. 300,2700")
    ap.add_argument("--bitrate", type=int)
    ap.add_argument("--seconds", type=float, default=5.0)
    a = ap.parse_args()

    u = urlparse(a.url)
    sock = socket.create_connection((u.hostname, u.port or 80), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET {u.path or '/'} HTTP/1.1\r\nHost: {u.hostname}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    r = Reader(sock)
    head = b""
    while b"\r\n\r\n" not in head:
        head += r.need(1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        sys.exit("handshake refused: " + head.decode(errors="replace"))

    if a.freq:
        send_frame(sock, 1, json.dumps({"cmd": "freq", "hz": a.freq}).encode())
    if a.filter:
        lo, hi = (float(x) for x in a.filter.split(","))
        send_frame(sock, 1, json.dumps({"cmd": "filter", "low": lo, "high": hi}).encode())
    if a.bitrate:
        send_frame(sock, 1, json.dumps({"cmd": "bitrate", "bps": a.bitrate}).encode())

    packets = audio_bytes = lost = 0
    last_seq = None
    t0 = time.time()
    while time.time() - t0 < a.seconds:
        op, payload = r.frame()
        if op == 1:
            print(payload.decode())
        elif op == 2 and payload[:1] == b"\x01":
            seq = struct.unpack(">H", payload[1:3])[0]
            if last_seq is not None:
                lost += (seq - last_seq - 1) & 0xFFFF
            last_seq = seq
            packets += 1
            audio_bytes += len(payload) - 3
        elif op == 8:
            break
    dt = time.time() - t0
    send_frame(sock, 8, b"\x03\xe8")
    print(f"audio: {packets} packets in {dt:.1f} s ({packets / dt:.1f}/s, expected 50), "
          f"{audio_bytes * 8 / dt / 1000:.1f} kbit/s, {lost} lost")
    return 0 if packets else 1


if __name__ == "__main__":
    sys.exit(main())
