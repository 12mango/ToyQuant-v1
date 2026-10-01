#!/usr/bin/env python3
"""Work with IEX HIST, the only free continuous depth feed this repository found that a Hong Kong individual can
download. **The route is rejected** -- see `data/README.md` and the data table in `docs/ROADMAP.md` -- and this
tool is kept because the container walk below is real work with a self-test tier 1 of the gate runs, and because
it is the first half of any decoder a future route would need.

What the route can and cannot do, which is the anatomy step every source in this repository went through:

* IEX HIST publishes one gzipped pcap per day per feed, named `<YYYYMMDD>_IEXTP1_DEEP1.0.pcap.gz` and
  `<YYYYMMDD>_IEXTP1_TOPS1.6.pcap.gz`, for the previous session, and it is free. DEEP is an **order-level** feed:
  order adds, executions, deletes and cancels, each carrying an order reference, rather than the aggregated price
  levels `l2_replay` consumes, so using it would mean a decoder plus an aggregation layer. TOPS carries the top
  of book only.
* **It is behind an account.** The archive is a download from IEX after a login and an acceptance of the IEX
  Historical Data Terms of Use, so no script can fetch it unattended and this one does not pretend to: it
  prints the procedure, and then reads whatever the operator downloaded.
* The DEEP message layout is not decoded here, and deliberately so. The published specification has moved off
  the URL this repository knew, and a decoder written from memory would produce a book that looks right and is
  not. What this tool does instead is check the parts the container itself describes: the gzip stream, the pcap
  records, the IP and UDP headers, the IEX-TP segment framing, and the message type bytes. Those are
  measurements on a real file, and they are what a decoder gets written against.

Usage:
    python3 tools/fetch_iex_hist.py --plan --date 2026-09-29
    python3 tools/fetch_iex_hist.py --verify data/v2/iex/20260929_IEXTP1_DEEP1.0.pcap.gz
    python3 tools/fetch_iex_hist.py --verify /tmp/down.pcap.gz --sample 4
    python3 tools/fetch_iex_hist.py --verify /tmp/down.pcap.gz --max-packets 100000   # a quick look
    python3 tools/fetch_iex_hist.py --self-test
"""

import argparse
import gzip
import io
import struct
import sys
import tempfile
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

PORTAL_URL = "https://iextrading.com/trading/market-data/"
FEEDS = {"DEEP": "DEEP1.0", "TOPS": "TOPS1.6"}
PCAP_MAGIC_US = b"\xd4\xc3\xb2\xa1"
PCAP_MAGIC_NS = b"\xa1\xb2\xc3\xd4"
# IEX-TP 1.0 carries a four byte prefix ahead of the message: version, reserved, then the message length as a
# big endian 16 bit count. The message's own first byte is its type, which is all this tool reads of it.
IEXTP_PREFIX = 4
ETHERTYPE_IPV4 = 0x0800
ETHERTYPE_VLAN = 0x8100
IPPROTO_UDP = 17


def archive_name(date, feed):
    return f"{date.replace('-', '')}_IEXTP1_{FEEDS[feed]}.pcap.gz"


def print_plan(date, feed):
    name = archive_name(date, feed)
    day = datetime.strptime(date, "%Y-%m-%d") + timedelta(days=1)
    print("[IEX_HIST] this route cannot be fetched unattended: the archive is behind an account and a licence")
    print(f"[IEX_HIST] 1. log in at {PORTAL_URL}")
    print("[IEX_HIST] 2. accept the IEX Historical Data Terms of Use (one time per account)")
    print(f"[IEX_HIST] 3. download the feed {feed}, which is published as {name}")
    print(f"[IEX_HIST] 4. save it under data/v2/iex/{name} (that directory is ignored by git)")
    print(f"[IEX_HIST] 5. run: python3 tools/fetch_iex_hist.py --verify data/v2/iex/{name}")
    print(f"[IEX_HIST] the file is the session of {date} in UTC and is published the day after, so "
          f"{day:%Y-%m-%d} is the first day it exists")
    print("[IEX_HIST] DEEP is the depth feed; TOPS is top of book only.")


def walk_packets(stream, sample, counters, max_packets=0):
    """Walk an Ethernet pcap stream down to its UDP payloads, counting what was seen along the way.

    `max_packets` stops the walk after that many packets, which is what makes the check usable on a whole day of
    DEEP without decompressing all of it. The report then says the stream was not read to its end, because the
    end is where a gzip file's checksum is.
    """
    header = stream.read(24)
    if len(header) < 24:
        raise SystemExit("the file is shorter than a pcap global header")
    magic = header[:4]
    if magic not in (PCAP_MAGIC_US, PCAP_MAGIC_NS):
        raise SystemExit(f"not a pcap stream: magic {magic.hex()}")
    # Microsecond and nanosecond variants both appear; the record timestamps are read the same way.
    counters["nanosecond_timestamps"] = magic == PCAP_MAGIC_NS
    major, minor, _, _, snaplen, link_type = struct.unpack("<HHiIII", header[4:])
    counters["pcap_version"] = f"{major}.{minor}"
    print(f"[IEX_HIST] pcap version {major}.{minor} snaplen {snaplen} link type {link_type}")
    if link_type != 1:
        print(f"[IEX_HIST] link type {link_type} is not Ethernet, which is the shape IEX publishes; stopping "
              "rather than guessing at the layer below")
        return

    first_ts = last_ts = None
    while not (max_packets and counters["packets"] >= max_packets):
        record = stream.read(16)
        if not record:
            break
        if len(record) < 16:
            counters["truncated_record"] = True
            break
        ts_sec, _, incl_len, orig_len = struct.unpack("<IIII", record)
        counters["packets"] += 1
        counters["frame_bytes"] += incl_len
        if incl_len < orig_len:
            counters["snaplen_truncated_frames"] += 1
        if first_ts is None:
            first_ts = ts_sec
        last_ts = ts_sec
        frame = stream.read(incl_len)
        if len(frame) < incl_len:
            counters["truncated_record"] = True
            break

        offset = 14
        if len(frame) < offset:
            counters["short_frames"] += 1
            continue
        ethertype = struct.unpack(">H", frame[12:14])[0]
        if ethertype == ETHERTYPE_VLAN:
            ethertype = struct.unpack(">H", frame[16:18])[0]
            offset += 4
        if ethertype != ETHERTYPE_IPV4:
            counters["non_ipv4_frames"] += 1
            continue
        if len(frame) < offset + 20:
            counters["short_frames"] += 1
            continue
        ihl = (frame[offset] & 0x0F) * 4
        if ihl < 20:
            counters["malformed_ipv4_frames"] += 1
            continue
        counters["ipv4_packets"] += 1
        if frame[offset + 9] != IPPROTO_UDP:
            counters["non_udp_packets"] += 1
            continue
        udp_offset = offset + ihl
        if len(frame) < udp_offset + 8:
            counters["short_frames"] += 1
            continue
        udp_len = struct.unpack(">H", frame[udp_offset + 4:udp_offset + 6])[0]
        counters["udp_packets"] += 1
        counters["udp_ports"][struct.unpack(">H", frame[udp_offset + 2:udp_offset + 4])[0]] += 1
        # The UDP length counts its own header, so the payload ends there rather than at the frame end, which
        # is where Ethernet padding lives.
        end = min(udp_offset + udp_len, len(frame))
        payload = frame[udp_offset + 8:end]
        if len(payload) < IEXTP_PREFIX:
            counters["short_payloads"] += 1
            continue
        message_length = struct.unpack(">H", payload[2:4])[0]
        counters["iextp_versions"][payload[0]] += 1
        counters["segment_lengths"][message_length] += 1
        # Whether a segment carries one message or several is a fact about the feed, so it is measured rather
        # than assumed: the remainder is what would follow the first message.
        counters["segment_remainder"][len(payload) - IEXTP_PREFIX - message_length] += 1
        if 1 <= message_length and IEXTP_PREFIX + message_length <= len(payload):
            counters["message_types"][payload[IEXTP_PREFIX]] += 1
        if sample > 0:
            sample -= 1
            counters["samples"].append((ts_sec, payload[:64].hex()))
    counters["first_ts"] = first_ts
    counters["last_ts"] = last_ts
    counters["stopped_early"] = bool(max_packets and counters["packets"] >= max_packets)



def new_counters():
    counters = Counter()
    for key in ("udp_ports", "iextp_versions", "segment_lengths", "segment_remainder", "message_types"):
        counters[key] = Counter()
    counters["samples"] = []
    return counters


def build_segment(message, ts_sec):
    """One pcap record holding one Ethernet frame, one IPv4 packet, one UDP datagram, one IEX-TP segment."""
    payload = struct.pack(">BBH", 1, 0, len(message)) + message
    udp = struct.pack(">HHHH", 12345, 40000, 8 + len(payload), 0) + payload
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0x4000, 64, IPPROTO_UDP, 0,
                     b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02") + udp
    frame = b"\x00" * 12 + struct.pack(">H", ETHERTYPE_IPV4) + ip
    return struct.pack("<IIII", ts_sec, 0, len(frame), len(frame)) + frame


def self_test():
    """Build a pcap in the container shape and check the walk reads back what went in.

    This proves the pcap, IPv4, UDP and IEX-TP framing walk, which is the part a decoder is built on top of and
    the part that can be checked without a file from IEX. It proves nothing about the DEEP message layout, which
    is exactly why no layout is implemented here. The timestamps and ports are the ones this builder sets, so a
    pass means the walk kept the fields apart rather than that the field meanings are right.
    """
    messages = [(b"D" + bytes(29), 1700000000), (b"S" + bytes(9), 1700000001), (b"P" + bytes(4), 1700000002)]
    records = [build_segment(message, ts) for message, ts in messages]
    raw = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1) + b"".join(records)
    counters = new_counters()
    walk_packets(io.BytesIO(raw), 0, counters)
    checks = [
        ("packets", counters["packets"] == 3),
        ("udp_packets", counters["udp_packets"] == 3),
        ("udp_port", dict(counters["udp_ports"]) == {40000: 3}),
        ("iextp_version", dict(counters["iextp_versions"]) == {1: 3}),
        ("message_lengths", dict(counters["segment_lengths"]) == {30: 1, 10: 1, 5: 1}),
        ("one_message_per_segment", dict(counters["segment_remainder"]) == {0: 3}),
        ("message_types", dict(counters["message_types"]) == {ord("D"): 1, ord("S"): 1, ord("P"): 1}),
        ("timestamps", (counters["first_ts"], counters["last_ts"]) == (1700000000, 1700000002)),
        ("no_problems", not any(counters.get(key) for key in
                                ("truncated_record", "short_payloads", "malformed_ipv4_frames",
                                 "snaplen_truncated_frames", "non_ipv4_frames", "non_udp_packets"))),
    ]
    # The gzip path is exercised end to end, because a truncated download is the failure this catches first.
    with tempfile.NamedTemporaryFile(suffix="_IEXTP1_DEEP1.0.pcap.gz", delete=False) as handle:
        handle.write(gzip.compress(raw))
        path = handle.name
    try:
        checks.append(("gzip_file_verifies", verify(path, 0) == 0))
    finally:
        Path(path).unlink()
    print(f"[IEX_HIST] self-test checks {len(checks)} passed {sum(1 for _, ok in checks if ok)}")
    for name, ok in checks:
        print(f"[IEX_HIST] self-test {name} {str(bool(ok)).lower()}")
    return 1 if any(not ok for _, ok in checks) else 0


def report(counters):
    def as_time(seconds):
        return datetime.fromtimestamp(seconds, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    print(f"[IEX_HIST] packets {counters['packets']} frame_bytes {counters['frame_bytes']} "
          f"udp_packets {counters['udp_packets']} complete_stream "
          f"{str(not counters.get('stopped_early')).lower()}")
    if counters["first_ts"] is not None:
        span = counters["last_ts"] - counters["first_ts"]
        print(f"[IEX_HIST] first_packet {as_time(counters['first_ts'])} last_packet "
              f"{as_time(counters['last_ts'])} span_hours {span / 3600.0:.3f}")
    print(f"[IEX_HIST] udp_ports {dict(counters['udp_ports'])}")
    print(f"[IEX_HIST] iextp_versions {dict(counters['iextp_versions'])}")
    print(f"[IEX_HIST] iextp_message_lengths {dict(counters['segment_lengths'])}")
    print(f"[IEX_HIST] iextp_segment_remainders {dict(counters['segment_remainder'])}")
    types = {chr(code) if 32 <= code < 127 else f"0x{code:02x}": count
             for code, count in sorted(counters["message_types"].items())}
    print(f"[IEX_HIST] message_type_bytes {types}")
    for ts, raw in counters["samples"]:
        print(f"[IEX_HIST] sample ts={as_time(ts)} payload={raw}")
    print("[IEX_HIST] the message contents above are not interpreted. The published DEEP specification has "
          "moved off the URL this repository knew, so no decoder is written from memory: the type bytes and "
          "lengths here are the column summary a decoder gets built against, and the samples are what to bring "
          "back to it.")
    problems = [key for key in ("truncated_record", "short_payloads", "malformed_ipv4_frames")
                if counters.get(key)]
    if counters.get("snaplen_truncated_frames"):
        problems.append("snaplen_truncated_frames")
    if not counters["udp_packets"] or not counters["message_types"]:
        problems.append("no_messages")
    if problems:
        print(f"[IEX_HIST] verify FAILED: {', '.join(problems)}")
        return 2
    one_message_per_segment = set(counters["segment_remainder"]) == {0}
    print(f"[IEX_HIST] verify OK: one_message_per_segment {str(one_message_per_segment).lower()} "
          f"messages {sum(counters['message_types'].values())}")
    return 0


def verify(path, sample, max_packets=0):
    counters = new_counters()
    source = Path(path)
    if not source.exists():
        raise SystemExit(f"{path} does not exist")
    with open(source, "rb") as probe:
        compressed = probe.read(2) == b"\x1f\x8b"
    print(f"[IEX_HIST] reading {path} ({source.stat().st_size} bytes on disk, "
          f"gzip {str(compressed).lower()})")
    # Reading a gzip file to its end is what verifies its CRC, so a truncated download is reported rather
    # than half decoded in silence.
    opener = gzip.open if compressed else open
    try:
        with opener(source, "rb") as stream:
            walk_packets(stream, sample, counters, max_packets)
    except (gzip.BadGzipFile, EOFError, OSError) as error:
        # A browser download that stopped early is the likeliest way this file arrives wrong, and the gzip
        # stream is what notices: the CRC lives at the end, so reading to the end is the check.
        print(f"[IEX_HIST] verify FAILED: the gzip stream is truncated or corrupt ({error})")
        return 2
    return report(counters)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--plan", action="store_true", help="print the download procedure for one day")
    parser.add_argument("--verify", metavar="FILE", help="check a downloaded pcap.gz and report its anatomy")
    parser.add_argument("--date", default=datetime.now(timezone.utc).strftime("%Y-%m-%d"),
                        help="session date for --plan, YYYY-MM-DD")
    parser.add_argument("--feed", default="DEEP", choices=sorted(FEEDS), help="feed for --plan")
    parser.add_argument("--sample", type=int, default=0,
                        help="print the raw first bytes of this many messages, for --verify")
    parser.add_argument("--max-packets", type=int, default=0,
                        help="stop --verify after this many packets, for a quick look at a whole day")
    parser.add_argument("--self-test", action="store_true",
                        help="build a pcap in the container shape and check the walk against it")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.plan:
        print_plan(args.date, args.feed)
        return 0
    if args.verify:
        return verify(args.verify, args.sample, args.max_packets)
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
