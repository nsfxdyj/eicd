#!/usr/bin/env python3
"""
mqtt_stub_broker.py - tiny MQTT 3.1.1 stub broker for eicd development.

Implements the server half of the publish path so the gateway can be tested
end to end without standing up a real broker:
  CONNECT -> CONNACK, PUBLISH QoS 0 / QoS 1 (-> PUBACK),
  PINGREQ -> PINGRESP, DISCONNECT.

Every published message is printed as one line:
  HH:MM:SS topic  payload

Not for production: no auth, no subscriptions, no TLS, no validation of
will topics, single global listener.

Usage: python3 mqtt_stub_broker.py [--host 127.0.0.1] [--port 1883]
"""
import argparse
import socket
import struct
import threading
import time


def read_packet(conn):
    """Return (header_byte, body) or None on EOF/protocol error."""
    hdr = conn.recv(1)
    if not hdr:
        return None
    mult, remaining = 1, 0
    for _ in range(4):
        b = conn.recv(1)
        if not b:
            return None
        remaining += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    else:
        return None
    body = b""
    while len(body) < remaining:
        chunk = conn.recv(remaining - len(body))
        if not chunk:
            return None
        body += chunk
    return hdr[0], body


def read_mstr(buf, off):
    n = struct.unpack(">H", buf[off:off + 2])[0]
    return buf[off + 2:off + 2 + n].decode("utf-8", "replace"), off + 2 + n


def serve(conn, addr):
    peer = f"{addr[0]}:{addr[1]}"
    try:
        while True:
            pkt = read_packet(conn)
            if pkt is None:
                break
            hdr, body = pkt
            ptype = hdr >> 4

            if ptype == 1:                          # CONNECT
                client_id, _ = read_mstr(body, 10)  # skip variable header
                print(f"{time.strftime('%H:%M:%S')} CONNECT    {peer} "
                      f"client_id={client_id}")
                conn.sendall(b"\x20\x02\x00\x00")   # CONNACK: accepted

            elif ptype == 3:                        # PUBLISH
                topic, off = read_mstr(body, 0)
                qos = (hdr >> 1) & 0x03
                pid = None
                if qos:
                    pid = body[off:off + 2]
                    off += 2
                payload = body[off:]
                print(f"{time.strftime('%H:%M:%S')} PUBLISH    {topic}  "
                      f"{payload.decode('utf-8', 'replace')}")
                if qos == 1 and pid:
                    conn.sendall(b"\x40\x02" + pid)  # PUBACK

            elif ptype == 12:                       # PINGREQ
                conn.sendall(b"\xd0\x00")           # PINGRESP

            elif ptype == 8:                        # DISCONNECT
                print(f"{time.strftime('%H:%M:%S')} DISCONNECT {peer}")
                break

    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    print(f"mqtt_stub_broker: listening on {args.host}:{args.port} "
          f"(ctrl-c to stop)")
    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=serve, args=(conn, addr),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
