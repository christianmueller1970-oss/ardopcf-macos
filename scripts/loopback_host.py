#!/usr/bin/env python3
"""Minimal non-interactive ardop host for scripts/test-loopback-macos.sh.

Connects to an ardopcf command port, sends the given host commands, then
keeps printing incoming command-port traffic ("<< ...") until killed.
Unlike host/python/diagnostichost.py this needs no TTY, so it can run in
the background from a shell script.

Usage: loopback_host.py <command-port> <command> [<command> ...]
"""
import socket
import sys
import threading
import time

port = int(sys.argv[1])
cmds = sys.argv[2:]

sock = socket.create_connection(("127.0.0.1", port), timeout=5)
sock.settimeout(0.2)


def reader():
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return
        if not data:
            return
        buf += data
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            print("<<", line.decode("ascii", "replace"), flush=True)


threading.Thread(target=reader, daemon=True).start()

for cmd in cmds:
    print(">>", cmd, flush=True)
    sock.sendall(cmd.encode("ascii") + b"\r")
    time.sleep(0.5)

while True:
    time.sleep(1)
