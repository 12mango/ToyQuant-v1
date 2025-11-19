#!/usr/bin/env python3
import socket
import time
import csv
import os

HOST = "127.0.0.1"
PORT = 9000
CSV_PATH = os.path.join("data","synthetic_ticks.csv")
MIN_DELAY = 0.0001  # 最小间隔 0.1ms 避免 CPU 占满

def load_csv(path):
    lines=[]
    with open(path,"r") as f:
        reader = csv.reader(f)
        for row in reader:
            if row and row[0].isdigit():
                lines.append(",".join(row))
    return lines

def send(host,port,lines):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prev_ts = None
    for line in lines:
        ts = int(line.split(",")[0])
        if prev_ts is not None:
            dt = (ts - prev_ts)/1000.0  # ms -> s
            time.sleep(max(dt, MIN_DELAY))
        s.sendto(line.encode(), (host, port))
        prev_ts = ts

if __name__=="__main__":
    lines = load_csv(CSV_PATH)
    send(HOST, PORT, lines)
