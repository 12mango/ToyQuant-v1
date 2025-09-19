#!/usr/bin/env python3
import socket
import time
import sys

def send(host,port,lines,delay=0.1):
    s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for l in lines:
        s.sendto(l.encode(), (host,port))
        time.sleep(delay)

if __name__=='__main__':
    if len(sys.argv)<3:
        print("usage: udp_sender.py <host> <port>")
        sys.exit(1)
    host=sys.argv[1]; port=int(sys.argv[2])
    lines=[
"1625097601000,EURUSD,1.1850,100,B",
"1625097601200,EURUSD,1.1851,60,S",
"1625097601400,EURUSD,1.1852,20,S",
"1625097601600,EURUSD,1.1849,300,B"
    ]
    send(host,port,lines,0.2)
