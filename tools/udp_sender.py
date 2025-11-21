#!/usr/bin/env python3
import socket
import time
import csv
import sys
from pathlib import Path

# === 默认参数 ===
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9000
DEFAULT_FILE = "data/synthetic_ticks.csv"
DEFAULT_DELAY = 0  # 毫秒延迟（0 表示按 CSV 时间戳间隔发送）

def read_ticks(file_path):
    ticks = []
    with open(file_path, "r") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or row[0].startswith("ts"):  # 跳过表头
                continue
            ticks.append(row)
    return ticks

def send_ticks(host, port, ticks, delay=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prev_ts = None
    for tick in ticks:
        ts, symbol, price, size, side = tick
        msg = ",".join([ts, symbol, price, size, side])
        s.sendto(msg.encode(), (host, port))
        if delay > 0:
            time.sleep(delay)
        elif prev_ts is not None:
            # 按 CSV 时间戳间隔发送，单位 ms
            sleep_sec = (int(ts) - prev_ts) / 1000.0
            if sleep_sec > 0:
                time.sleep(sleep_sec)
        prev_ts = int(ts)
    s.close()

if __name__ == "__main__":
    host = DEFAULT_HOST
    port = DEFAULT_PORT
    file_path = DEFAULT_FILE
    delay = DEFAULT_DELAY

    # 可选命令行参数：host port file delay(ms)
    if len(sys.argv) > 1:
        host = sys.argv[1]
    if len(sys.argv) > 2:
        port = int(sys.argv[2])
    if len(sys.argv) > 3:
        file_path = sys.argv[3]
    if len(sys.argv) > 4:
        delay = float(sys.argv[4]) / 1000.0  # 转成秒

    file_path = Path(file_path)
    if not file_path.exists():
        print(f"Tick file not found: {file_path}")
        sys.exit(1)

    ticks = read_ticks(file_path)
    print(f"Sending {len(ticks)} ticks to {host}:{port}...")
    send_ticks(host, port, ticks, delay)
    print("Done.")
