#!/usr/bin/env python3
"""Mibot UART 测试对端：模拟 SF32 与 ESP32 固件对话。

用法：
  python mibot_uart_peer.py COM5 hello          # 发 HELLO，等待 HELLO_ACK
  python mibot_uart_peer.py COM5 ping 10        # 发 10 帧 PING，统计 RTT
  python mibot_uart_peer.py COM5 telemetry 20   # 观察 20 秒 TELEMETRY
需要: pip install pyserial
"""
import json
import struct
import sys
import time

import serial

SOF = b"\xAA\x55"
VERSION = 0x01

TYPE_HELLO, TYPE_HELLO_ACK = 0x01, 0x02
TYPE_TELEMETRY = 0x21
TYPE_PING, TYPE_PONG = 0x60, 0x61

FLAG_ACK_REQUEST, FLAG_ACK, FLAG_ERROR, FLAG_FRAGMENT = 0x01, 0x02, 0x04, 0x08


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, flags: int, seq: int, payload: bytes) -> bytes:
    body = struct.pack("<BBBHH", VERSION, ftype, flags, seq, len(payload)) + payload
    return SOF + body + struct.pack("<H", crc16_ccitt_false(body))


class Decoder:
    """增量解码：feed(新字节) → 返回本次解出的 (type, flags, seq, payload) 列表"""

    def __init__(self):
        self.buf = b""

    def feed(self, data: bytes):
        self.buf += data
        frames = []
        while True:
            start = self.buf.find(SOF)
            if start < 0:
                self.buf = b""
                break
            self.buf = self.buf[start:]
            if len(self.buf) < 9:  # SOF(2) + 头(7)
                break
            ver, ftype, flags, seq, length = struct.unpack("<BBBHH", self.buf[2:9])
            if ver != VERSION or length > 4096:
                self.buf = self.buf[1:]  # 假 SOF / 坏头：滑动 1 字节重新同步
                continue
            frame_len = 11 + length  # SOF(2) + 头(7) + payload + CRC(2)
            if len(self.buf) < frame_len:
                break  # 半帧，等更多数据
            payload = self.buf[9:9 + length]
            (crc,) = struct.unpack("<H", self.buf[9 + length:frame_len])
            expect = crc16_ccitt_false(self.buf[2:9 + length])
            self.buf = self.buf[frame_len:]
            if crc != expect:
                print(f"[decode] CRC error type=0x{ftype:02x}")
                continue
            frames.append((ftype, flags, seq, payload))
        return frames


def main():
    port, command = sys.argv[1], sys.argv[2]
    arg = sys.argv[3] if len(sys.argv) > 3 else None
    ser = serial.Serial(port, 921600, timeout=0.1)
    dec = Decoder()

    def rx():
        return dec.feed(ser.read(4096))

    if command == "hello":
        payload = json.dumps({"schema": "mibot.uart.v1", "fw_version": "peer-sim-0.1",
                              "proto_version": 1, "device_id": "sf32-sim"}).encode()
        ser.write(encode(TYPE_HELLO, FLAG_ACK_REQUEST, 1, payload))
        deadline = time.time() + 3
        while time.time() < deadline:
            for ftype, flags, seq, payload in rx():
                if ftype == TYPE_HELLO_ACK:
                    print(f"HELLO_ACK seq={seq} flags=0x{flags:02x}: {payload.decode()}")
                    return
        sys.exit("no HELLO_ACK within 3s")

    elif command == "ping":
        count = int(arg or 10)
        rtts = []
        for i in range(count):
            t0 = time.time()
            ser.write(encode(TYPE_PING, FLAG_ACK_REQUEST, i, b""))
            got = False
            while time.time() - t0 < 2 and not got:
                for ftype, flags, seq, _ in rx():
                    if ftype == TYPE_PONG and seq == i:
                        rtts.append((time.time() - t0) * 1000)
                        got = True
            if not got:
                print(f"ping {i}: TIMEOUT")
        if rtts:
            print(f"rtt min/avg/max = {min(rtts):.1f}/{sum(rtts)/len(rtts):.1f}/{max(rtts):.1f} ms ({len(rtts)}/{count})")

    elif command == "telemetry":
        seconds = int(arg or 20)
        deadline = time.time() + seconds
        while time.time() < deadline:
            for ftype, flags, seq, payload in rx():
                if ftype == TYPE_TELEMETRY:
                    print(f"[{time.strftime('%H:%M:%S')}] {payload.decode()}")
                elif ftype == TYPE_HELLO_ACK:
                    print("HELLO_ACK received — link ready")


if __name__ == "__main__":
    main()
