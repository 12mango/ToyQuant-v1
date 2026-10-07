#!/usr/bin/env python3
"""A local WebSocket sender, the live twin of tools/udp_sender.py.

It stands in for a venue's streaming endpoint so the WebSocketFeed client can be exercised on a
desk with no exchange and no credential. It is a WebSocket server, not a client: it accepts one
connection, completes the opening handshake, then streams the two replay CSV files it is given as
text frames, one row per message, interleaved by their own timestamps so the two queues inside the
feed see the stream in the order the file's clock implies.

The rows are the shapes the client already reads, and nothing is transformed: a trade file and a
depth file written by `--record`, or any pair in the same shapes, stream back unchanged. That is the
point -- a session recorded from a replay replays again over this feed.

Usage:
    python3 tools/websocket_sender.py [--host H] [--port P] --trades FILE --depth FILE [--speed X]

--speed is a multiple of wall clock: 0 (the default) sends as fast as the socket accepts, which is
what a test wants; 1.0 reproduces the file's own timing, which is what a rehearsal wants.
"""

import argparse
import base64
import hashlib
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_rows(path):
    """Every data row of a replay CSV, with its header and blank lines dropped."""
    rows = []
    with open(path, "r") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line.startswith("exchange,"):
                continue
            rows.append(line)
    return rows


def timestamp_of(row):
    # Both replay shapes carry the exchange timestamp as their third column.
    try:
        return int(row.split(",")[2])
    except (IndexError, ValueError):
        return 0


def merged_stream(trades, depth):
    """The two row lists merged by their exchange timestamp, ties going to the depth row.

    The merge is what keeps the sender honest: a feed whose trade queue fills while it waits for a
    first depth row would deadlock, and a venue never sends all of one stream before the other.
    """
    merged = []
    i = j = 0
    while i < len(trades) and j < len(depth):
        if timestamp_of(depth[j]) <= timestamp_of(trades[i]):
            merged.append(depth[j])
            j += 1
        else:
            merged.append(trades[i])
            i += 1
    merged.extend(trades[i:])
    merged.extend(depth[j:])
    return merged


def encode_frame(payload, opcode=0x1):
    """One unmasked server-to-client WebSocket frame (RFC 6455 section 5.2)."""
    header = bytearray()
    header.append(0x80 | opcode)
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    return bytes(header) + payload


def handshake(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        data += chunk
    key = None
    for line in data.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    if not key:
        return False
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    conn.sendall(response.encode("latin-1"))
    return True


def serve(host, port, rows, speed):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, port))
    listener.listen(1)
    print(f"listening on ws://{host}:{port}, {len(rows)} rows", flush=True)
    conn, peer = listener.accept()
    try:
        if not handshake(conn):
            print("handshake failed", file=sys.stderr)
            return 1
        print(f"client {peer[0]}:{peer[1]} connected", flush=True)
        previous_ts = None
        for row in rows:
            conn.sendall(encode_frame((row + "\n").encode()))
            if speed > 0 and previous_ts is not None:
                delay = (timestamp_of(row) - previous_ts) / 1000.0 / speed
                if delay > 0:
                    time.sleep(delay)
            previous_ts = timestamp_of(row)
        # A close frame, then the socket: the client's readers drain what arrived and stop.
        conn.sendall(encode_frame(b"", opcode=0x8))
        print("sent every row", flush=True)
    finally:
        conn.close()
        listener.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--trades", required=True)
    parser.add_argument("--depth", required=True)
    parser.add_argument("--speed", type=float, default=0.0)
    args = parser.parse_args()

    trades = read_rows(args.trades)
    depth = read_rows(args.depth)
    if not trades and not depth:
        print("neither input file held a data row", file=sys.stderr)
        return 1
    return serve(args.host, args.port, merged_stream(trades, depth), args.speed)


if __name__ == "__main__":
    sys.exit(main())
