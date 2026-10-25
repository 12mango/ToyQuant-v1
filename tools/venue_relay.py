#!/usr/bin/env python3
"""Relay a real venue's public market data to the engine: a client to the venue, a server to the engine.

This is the interchangeable half of the live path. It opens the venue's public stream (a **client**,
over `wss://`, no API key), decodes the venue's own message shapes into the replay CSV rows this
repository already reads, and re-serves those rows as plaintext `ws://` on localhost (a **server**),
where the C++ `WebSocketFeed` reads them exactly as it reads the local sender in tests. So the engine
does not know, and does not need to know, whether the row came from a file, from the local sender or
from a venue -- and swapping this Python relay for a C++ one that speaks `wss://` itself changes
nothing on the engine side.

The first venue is OKX's public channel, because it needs no key and its historical endpoint carries no
order book, so a live socket is its only keyless programmatic book route. Two channels are used:

    trades   one row per print:  exchange,symbol,timestamp,id,side,price,amount
    books    the incremental book: one row per level change,
             exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount

`books` opens with a full snapshot and then sends touched levels, where a size of zero removes one, and
every message carries `seqId` and `prevSeqId`, which is what a gap in the stream is detected against.
Sizes are contract counts, so a quantity is `round(size * contract_size * quantity_scale)`, the same
arithmetic `src/market/okx_readers.cpp` applies to the archive.

Usage:
    # serve the venue to the engine (start this first, then point the engine at it)
    python3 tools/venue_relay.py --instrument BTC-USDT-SWAP --listen 127.0.0.1:8765
    ./out/build/linux-debug/toy_quant ws 127.0.0.1:8765 BTC-USDT-SWAP active_l2 1 prorata --fast-validation

    # capture the venue's messages for an offline run, then serve them back with no network
    python3 tools/venue_relay.py --capture /tmp/okx.jsonl --max-seconds 60
    python3 tools/venue_relay.py --replay /tmp/okx.jsonl --listen 127.0.0.1:8765

    python3 tools/venue_relay.py --self-test
"""

import argparse
import base64
import hashlib
import json
import os
import queue
import socket
import ssl
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
DEFAULT_VENUE = "wss://ws.okx.com:8443/ws/v5/public"
DEFAULT_INSTRUMENT = "BTC-USDT-SWAP"
DEFAULT_CONTRACT_SIZE = 0.01
DEFAULT_QUANTITY_SCALE = 1000000
# OKX closes an idle public connection, and asks for a text ping when nothing has been sent for 30 s.
PING_AFTER_SECONDS = 25
# Rows in flight between the venue thread and the thread serving the engine. Bounded, because an engine
# that falls behind must slow the venue down rather than grow this process without limit; the wait is
# counted and printed, so a stalled consumer is visible in the run's own summary rather than only as RSS.
ROW_QUEUE_CAPACITY = 200000


# ---------------------------------------------------------------- WebSocket framing

def encode_frame(payload, opcode=0x1):
    """One unmasked server-to-client frame; a server never masks."""
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


def send_masked(sock, text, opcode=0x1):
    """One masked client-to-server frame; a client always masks."""
    data = text.encode() if isinstance(text, str) else text
    mask = os.urandom(4)
    header = bytearray([0x80 | opcode])
    length = len(data)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    header += mask
    sock.sendall(bytes(header) + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))


def recv_exact(sock, count):
    buffer = b""
    while len(buffer) < count:
        chunk = sock.recv(count - len(buffer))
        if not chunk:
            return None
        buffer += chunk
    return buffer


def read_frame(sock):
    """One frame off a venue connection, unmasked (a server never masks). None at close."""
    header = recv_exact(sock, 2)
    if header is None:
        return None
    fin = bool(header[0] & 0x80)
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    if mask:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return fin, opcode, payload


def websocket_client(url):
    """Opens a `wss://` or `ws://` connection and completes the handshake."""
    secure = url.startswith("wss://")
    rest = url.split("://", 1)[1]
    host_port, _, path = rest.partition("/")
    host, _, port_text = host_port.partition(":")
    port = int(port_text) if port_text else (443 if secure else 80)
    path = "/" + path if path else "/"
    raw = socket.create_connection((host, port), timeout=15)
    sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) if secure else raw
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        f"User-Agent: Mozilla/5.0 toyquant-relay/1.0\r\n\r\n"
    )
    sock.sendall(request.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(1)
    if b" 101 " not in response.split(b"\r\n", 1)[0]:
        raise RuntimeError("venue refused the websocket handshake: " + response.split(b"\r\n")[0].decode())
    return sock



# ---------------------------------------------------------------- decoding a venue message

TRADE_HEADER = "exchange,symbol,timestamp,id,side,price,amount\n"
DEPTH_HEADER = "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"


class Decoder:
    """One venue connection's message decoder: JSON in, replay CSV rows out.

    It also owns the sequence bookkeeping, because `books` carries the pair `prevSeqId`/`seqId` and a
    break between them is the one thing a live book cannot recover from on its own -- it is a lost
    update, and the count is what "recorded for hours without a drop" is measured against.
    """

    def __init__(self, instrument, contract_size, quantity_scale, max_levels):
        self.exchange = "okx"
        self.instrument = instrument
        self.contract_size = contract_size
        self.quantity_scale = quantity_scale
        self.max_levels = max_levels
        self.last_seq = None
        self.trades = 0
        self.depth_rows = 0
        self.book_messages = 0
        self.gaps = 0

    def amount(self, size_text):
        return int(round(float(size_text) * self.contract_size * self.quantity_scale))

    def decode(self, text):
        """The rows for one venue message; empty for an event, a ping, or a channel we ignore."""
        try:
            message = json.loads(text)
        except ValueError:
            return []
        arg = message.get("arg", {})
        channel = arg.get("channel")
        symbol = arg.get("instId", self.instrument)
        if channel == "trades":
            return self._trades(message, symbol)
        if channel in ("books", "books5", "bbo-tbt"):
            return self._books(message, symbol)
        return []

    def _trades(self, message, symbol):
        rows = []
        for trade in message.get("data", []):
            ts = trade.get("ts")
            if ts is None:
                continue
            rows.append(
                f"{self.exchange},{symbol},{ts},{trade.get('tradeId','')},"
                f"{trade.get('side','')},{trade.get('px','')},{self.amount(trade.get('sz','0'))}\n"
            )
            self.trades += 1
        return rows

    def _books(self, message, symbol):
        action = message.get("action", "snapshot")
        is_snapshot = 1 if action == "snapshot" else 0
        rows = []
        for book in message.get("data", []):
            seq = book.get("seqId")
            prev = book.get("prevSeqId")
            if is_snapshot:
                self.last_seq = seq
            else:
                # A gap is a missing update: the book's `prevSeqId` is not the `seqId` we last saw.
                if self.last_seq is not None and prev is not None and int(prev) != int(self.last_seq):
                    self.gaps += 1
                self.last_seq = seq
            ts = book.get("ts", "0")
            # A snapshot is capped to the levels the engine uses; an update is sparse already and is
            # passed through whole so a level the snapshot kept is never left stale.
            for side, levels in (("bid", book.get("bids", [])), ("ask", book.get("asks", []))):
                if is_snapshot and self.max_levels and len(levels) > self.max_levels:
                    levels = levels[: self.max_levels]
                for level in levels:
                    price, size = level[0], level[1]
                    rows.append(
                        f"{self.exchange},{symbol},{ts},{ts},{is_snapshot},{side},"
                        f"{price},{self.amount(size)}\n"
                    )
                    self.depth_rows += 1
            self.book_messages += 1
        return rows

    def summary(self):
        return (
            f"trades={self.trades} book_messages={self.book_messages} depth_rows={self.depth_rows} "
            f"gaps={self.gaps}"
        )


class Recorder:
    """The direct route: the decoded rows written straight to the replay CSV pair, no relay and no
    engine. It is the counterpart of the relay's server role -- one connection to the venue, the same
    decoder, the same rows -- and it is what makes a live window a file this repository can replay."""

    def __init__(self, prefix):
        self.trades = open(prefix + "_trades.csv", "w", encoding="utf-8")
        self.depth = open(prefix + "_depth.csv", "w", encoding="utf-8")
        self.trades.write(TRADE_HEADER)
        self.depth.write(DEPTH_HEADER)
        self.rows = 0

    def write(self, row):
        # A trade row has seven fields, a depth row eight, which is how the reader tells them apart.
        (self.trades if row.count(",") == 6 else self.depth).write(row)
        self.rows += 1
        # A long recording is flushed as it goes, so a session that is stopped rather than closed loses
        # at most this many rows instead of whatever the file object was holding.
        if self.rows % 4096 == 0:
            self.trades.flush()
            self.depth.flush()

    def close(self):
        self.trades.close()
        self.depth.close()


# ---------------------------------------------------------------- the venue client and the local server

class VenueClient(threading.Thread):
    """A client to the venue: subscribe, then decode every message into the row queue."""

    def __init__(self, url, instrument, books_channel, decoder, rows, capture=None, max_seconds=0,
                 enqueue=True, recorder=None):
        super().__init__(daemon=True)
        self.url = url
        self.instrument = instrument
        self.books_channel = books_channel
        self.decoder = decoder
        self.rows = rows
        self.capture = capture
        self.max_seconds = max_seconds
        self.enqueue = enqueue
        self.recorder = recorder
        self.stop = threading.Event()
        self.connected = False
        self.reconnects = 0
        # Backpressure and heartbeat, both of which a six second run never exercises.
        self.queue_waits = 0
        self.queue_high_water = 0
        self.pings_sent = 0
        self.pongs_sent = 0

    def subscription(self):
        return json.dumps({
            "op": "subscribe",
            "args": [{"channel": self.books_channel, "instId": self.instrument},
                     {"channel": "trades", "instId": self.instrument}],
        })

    def enqueue_row(self, row):
        """Hands a row to the server thread, waiting for room rather than dropping it.

        A full queue is backpressure: the venue connection stops being read until the engine catches
        up, which is the same rule the engine's own rings follow. The wait and the high-water mark are
        counted so a slow consumer is visible in the summary instead of only as growing memory.
        """
        while not self.stop.is_set():
            try:
                self.rows.put(row, timeout=0.2)
                self.queue_high_water = max(self.queue_high_water, self.rows.qsize())
                return
            except queue.Full:
                self.queue_waits += 1

    def run(self):
        """Connect, and reconnect with a backoff after a drop, until stopped or out of time."""
        deadline = time.time() + self.max_seconds if self.max_seconds else None
        sink = open(self.capture, "a") if self.capture else None
        backoff = 1.0
        try:
            while not self.stop.is_set():
                if deadline is not None and time.time() >= deadline:
                    break
                try:
                    self.session(sink, deadline)
                    backoff = 1.0
                except (OSError, RuntimeError) as error:
                    if self.stop.is_set():
                        break
                    self.reconnects += 1
                    print(f"venue connection lost ({error}); reconnecting in {backoff:.0f}s", flush=True)
                if self.stop.is_set():
                    break
                if deadline is not None and time.time() >= deadline:
                    break
                time.sleep(backoff)
                backoff = min(backoff * 2.0, 8.0)
        finally:
            if sink is not None:
                sink.close()
            if self.enqueue:
                self.rows.put(None)  # end of stream, so a waiting server stops cleanly

    def session(self, sink, deadline):
        """One venue connection, handshake to drop. Returns at a clean close or the deadline; a
        transport failure is raised so run() can reconnect. A reconnect is safe for the book because
        the venue re-sends a full snapshot on subscribe, which resets the decoder's sequence."""
        sock = websocket_client(self.url)
        self.connected = True
        try:
            send_masked(sock, self.subscription())
            sock.settimeout(PING_AFTER_SECONDS)
            while not self.stop.is_set():
                if deadline is not None and time.time() >= deadline:
                    return
                try:
                    frame = read_frame(sock)
                except socket.timeout:
                    send_masked(sock, "ping")  # keep an idle public connection alive
                    self.pings_sent += 1
                    continue
                if frame is None:
                    return
                _, opcode, payload = frame
                if opcode == 0x8:
                    return
                if opcode == 0x9:
                    # A control ping is owed a control pong with the same payload, masked as a client's
                    # frames must be. A session that answers neither this nor the text form is closed
                    # by the venue once its heartbeat times out, which is exactly the failure a six
                    # second run cannot see.
                    send_masked(sock, payload, opcode=0xA)
                    self.pongs_sent += 1
                    continue
                if opcode == 0xA:
                    continue
                if opcode not in (0x1, 0x0):
                    continue
                text = payload.decode(errors="replace")
                token = text.strip()
                # OKX's heartbeat is a text frame rather than a control frame, and it is answered the
                # same way: a pong for a ping, and neither is a market message to record.
                if token == "ping":
                    send_masked(sock, "pong")
                    self.pongs_sent += 1
                    continue
                if token == "pong":
                    continue
                if sink is not None:
                    sink.write(text + "\n")
                    sink.flush()
                for row in self.decoder.decode(text):
                    if self.recorder is not None:
                        self.recorder.write(row)
                    if self.enqueue:
                        self.enqueue_row(row)
        finally:
            sock.close()


def accept_client(listen):
    """Blocks until the engine connects, completes the handshake, and returns the socket."""
    host, _, port = listen.partition(":")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, int(port)))
    listener.listen(1)
    print(f"relay listening on ws://{listen}, waiting for the engine ...", flush=True)
    conn, peer = listener.accept()
    request = b""
    while b"\r\n\r\n" not in request:
        request += conn.recv(4096)
    key = ""
    for line in request.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
    ).encode("latin-1"))
    print(f"engine connected from {peer[0]}:{peer[1]}", flush=True)
    return listener, conn


def stream(conn, rows, stop):
    """Forwards rows to the engine until the source ends or the engine goes away."""
    conn.sendall(encode_frame(TRADE_HEADER.encode()))
    conn.sendall(encode_frame(DEPTH_HEADER.encode()))
    sent = 0
    while True:
        try:
            row = rows.get(timeout=0.2)
        except queue.Empty:
            if stop.is_set():
                break
            continue
        if row is None:
            break
        try:
            conn.sendall(encode_frame(row.encode()))
        except OSError:
            break
        sent += 1
    try:
        conn.sendall(encode_frame(b"", opcode=0x8))
    except OSError:
        pass
    return sent



# ---------------------------------------------------------------- self-test and main

def decode_file(path, decoder):
    """Rows for a captured file: an offline run with no network at all."""
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield from decoder.decode(line)


def self_test():
    """Decode the shapes a live capture showed, and check the rows and the gap count."""
    decoder = Decoder("BTC-USDT-SWAP", DEFAULT_CONTRACT_SIZE, DEFAULT_QUANTITY_SCALE, 1)

    trade = ('{"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":['
             '{"instId":"BTC-USDT-SWAP","tradeId":"7","px":"100.5","sz":"2.5",'
             '"side":"buy","ts":"1000","seqId":5}]}')
    rows = decoder.decode(trade)
    assert rows == ["okx,BTC-USDT-SWAP,1000,7,buy,100.5,25000\n"], rows

    # max_levels is 1, so one level a side, both marked as the snapshot's.
    snapshot = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":['
                '{"asks":[["100.6","3","0","1"],["100.7","1","0","1"]],'
                '"bids":[["100.4","5","0","2"],["100.3","2","0","2"]],'
                '"ts":"1000","seqId":10,"prevSeqId":-1}]}')
    assert decoder.decode(snapshot) == [
        "okx,BTC-USDT-SWAP,1000,1000,1,bid,100.4,50000\n",
        "okx,BTC-USDT-SWAP,1000,1000,1,ask,100.6,30000\n",
    ]
    assert decoder.last_seq == 10

    # A removal: size zero, and the sequence continues, so no gap is counted.
    update = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":['
              '{"asks":[["100.6","0","0","0"]],"bids":[],"ts":"1001","seqId":11,"prevSeqId":10}]}')
    assert decoder.decode(update) == ["okx,BTC-USDT-SWAP,1001,1001,0,ask,100.6,0\n"]
    assert decoder.gaps == 0, decoder.gaps

    # A break in the sequence is a lost update, and it is counted rather than hidden.
    gap = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":['
           '{"asks":[],"bids":[],"ts":"1002","seqId":99,"prevSeqId":42}]}')
    decoder.decode(gap)
    assert decoder.gaps == 1, decoder.gaps

    # An event and a non-JSON line are ignored rather than crashing.
    assert decoder.decode('{"event":"subscribe","arg":{"channel":"books"},"connId":"x"}') == []
    assert decoder.decode("pong") == []

    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--venue", default=DEFAULT_VENUE, help="the venue's public websocket url")
    parser.add_argument("--instrument", default=DEFAULT_INSTRUMENT)
    parser.add_argument("--books-channel", default="books", choices=["books", "books5", "bbo-tbt"])
    parser.add_argument("--levels", type=int, default=25,
                        help="levels kept from a snapshot; 0 keeps every level")
    parser.add_argument("--contract-size", type=float, default=DEFAULT_CONTRACT_SIZE)
    parser.add_argument("--quantity-scale", type=int, default=DEFAULT_QUANTITY_SCALE)
    parser.add_argument("--listen", default="127.0.0.1:8765",
                        help="where to serve the engine; empty to only capture")
    parser.add_argument("--capture", help="write the venue's raw messages here, for a later --replay")
    parser.add_argument("--record", help="write the decoded rows straight to PREFIX_trades.csv and "
                                         "PREFIX_depth.csv: the direct route, with no engine")
    parser.add_argument("--replay", help="serve this capture instead of connecting; no network")
    parser.add_argument("--max-seconds", type=float, default=0.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    decoder = Decoder(args.instrument, args.contract_size, args.quantity_scale, args.levels)
    stop = threading.Event()
    # Bounded for a live relay, where a stalled engine must slow the venue down. A replay reads a finite
    # file, so it keeps the unbounded queue and never blocks on a consumer that has already gone away.
    rows = queue.Queue(maxsize=ROW_QUEUE_CAPACITY if args.listen and not args.replay else 0)

    if args.replay:
        recorder = Recorder(args.record) if args.record else None
        for row in decode_file(args.replay, decoder):
            if recorder is not None:
                recorder.write(row)
            rows.put(row)
        if recorder is not None:
            recorder.close()
        rows.put(None)
        if not args.listen:
            print(decoder.summary(), flush=True)
            return 0
        listener, conn = accept_client(args.listen)
        try:
            stream(conn, rows, stop)
        finally:
            conn.close()
            listener.close()
        print(decoder.summary(), flush=True)
        return 0

    if args.listen:
        listener, conn = accept_client(args.listen)
    else:
        listener, conn = None, None

    recorder = Recorder(args.record) if args.record else None
    client = VenueClient(args.venue, args.instrument, args.books_channel, decoder, rows,
                         capture=args.capture, max_seconds=args.max_seconds, enqueue=conn is not None,
                         recorder=recorder)
    client.start()
    try:
        if conn is None:
            client.join()  # capture only: run until the deadline or the venue closes
        else:
            stream(conn, rows, stop)
    except KeyboardInterrupt:
        pass
    finally:
        client.stop.set()
        client.join(timeout=2)
        if recorder is not None:
            recorder.close()
        if conn is not None:
            conn.close()
        if listener is not None:
            listener.close()
    if args.capture:
        print(f"captured the venue's messages to {args.capture}", flush=True)
    if args.record:
        print(f"recorded the decoded rows to {args.record}_trades.csv and {args.record}_depth.csv",
              flush=True)
    print(f"{decoder.summary()} reconnects={client.reconnects} "
          f"queue_high_water={client.queue_high_water} queue_waits={client.queue_waits} "
          f"pings_sent={client.pings_sent} pongs_sent={client.pongs_sent}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

