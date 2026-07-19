#!/usr/bin/env python3
"""
modbus_sim.py - minimal Modbus TCP server for eicd development and testing.

Implements just enough of the protocol to exercise the gateway end to end:
  FC 0x01 read coils          FC 0x03 read holding registers
  FC 0x02 read discrete inputs  FC 0x04 read input registers
  FC 0x06 write single register

A background thread animates a small process model so the gateway sees
realistic, continuously changing data (matching etc/points.csv.example):

  holding[100:102]  float32  reactor temperature   ~80 +/- 5 degC
  holding[102:104]  float32  reactor pressure      ~4.0 +/- 0.5 bar
  holding[104:106]  uint32   cycle counter         +1 every 2 s
  holding[106]      uint16   motor speed           ~1450 rpm
  input[200]        uint16   valve position        0..100 %
  coil[5]           bool     pump running          toggles every 1 s
  discrete[8]       bool     high-temperature alarm (temp > 83 degC)

Usage: python3 modbus_sim.py [--host 127.0.0.1] [--port 1502] [-v]
"""
import argparse
import math
import socket
import struct
import threading
import time


class ModbusSim:
    """Register/bitmap store plus request dispatcher. Thread-safe."""

    def __init__(self):
        self.lock = threading.Lock()
        self.holding = {i: 0 for i in range(0, 200)}
        self.inputs = {i: 0 for i in range(200, 210)}
        self.coils = {i: 0 for i in range(0, 16)}
        self.discrete = {i: 0 for i in range(0, 16)}
        self.running = True
        threading.Thread(target=self._animate, daemon=True).start()

    # ---- process model -------------------------------------------------

    @staticmethod
    def _set_f32(table, addr, value):
        packed = struct.pack(">f", value)
        table[addr] = struct.unpack(">H", packed[0:2])[0]
        table[addr + 1] = struct.unpack(">H", packed[2:4])[0]

    @staticmethod
    def _set_u32(table, addr, value):
        table[addr] = (value >> 16) & 0xFFFF
        table[addr + 1] = value & 0xFFFF

    def _animate(self):
        cycles = 0
        t0 = time.time()
        while self.running:
            t = time.time() - t0
            temp = 80.0 + 5.0 * math.sin(t / 10.0)
            with self.lock:
                self._set_f32(self.holding, 100, temp)
                self._set_f32(self.holding, 102, 4.0 + 0.5 * math.sin(t / 3.0))
                self._set_u32(self.holding, 104, cycles)
                self.holding[106] = int(1450 + 50 * math.sin(t / 7.0))
                self.inputs[200] = int(50 + 50 * math.sin(t / 9.0))
                self.coils[5] = int(t) % 2
                self.discrete[8] = 1 if temp > 83.0 else 0
            cycles += 1
            time.sleep(2.0)

    # ---- protocol --------------------------------------------------------

    @staticmethod
    def _exc(func, code):
        return bytes([func | 0x80, code])

    def handle(self, pdu):
        func = pdu[0]
        try:
            if func in (3, 4):                      # read registers
                addr, qty = struct.unpack(">HH", pdu[1:5])
                table = self.holding if func == 3 else self.inputs
                with self.lock:
                    if qty < 1 or qty > 125 or any(
                            (addr + i) not in table for i in range(qty)):
                        return self._exc(func, 2)   # illegal data address
                    data = b"".join(struct.pack(">H", table[addr + i])
                                    for i in range(qty))
                return bytes([func, len(data)]) + data

            if func in (1, 2):                      # read bits
                addr, qty = struct.unpack(">HH", pdu[1:5])
                table = self.coils if func == 1 else self.discrete
                nbytes = (qty + 7) // 8
                out = bytearray(nbytes)
                with self.lock:
                    if qty < 1 or qty > 1968:
                        return self._exc(func, 2)
                    for i in range(qty):
                        if (addr + i) not in table:
                            return self._exc(func, 2)
                        if table[addr + i]:
                            out[i // 8] |= 1 << (i % 8)
                return bytes([func, nbytes]) + bytes(out)

            if func == 6:                           # write single register
                addr, val = struct.unpack(">HH", pdu[1:5])
                with self.lock:
                    if addr not in self.holding:
                        return self._exc(func, 2)
                    self.holding[addr] = val
                return pdu[:5]                      # FC6 echoes the request

            return self._exc(func, 1)               # illegal function
        except struct.error:
            return self._exc(func, 3)               # illegal data value


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def serve_client(conn, addr, sim, verbose):
    peer = f"{addr[0]}:{addr[1]}"
    try:
        while True:
            hdr = recv_exact(conn, 7)               # MBAP header
            if hdr is None:
                break
            tid, _pid, length, unit = struct.unpack(">HHHB", hdr)
            if length < 2:
                break
            pdu = recv_exact(conn, length - 1)
            if pdu is None:
                break
            if verbose:
                print(f"<- {peer} unit={unit} pdu={pdu.hex()}")
            resp = sim.handle(pdu)
            conn.sendall(struct.pack(">HHHB", tid, 0, len(resp) + 1, unit) + resp)
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1502)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    sim = ModbusSim()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    print(f"modbus_sim: listening on {args.host}:{args.port} (ctrl-c to stop)")
    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=serve_client,
                             args=(conn, addr, sim, args.verbose),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
