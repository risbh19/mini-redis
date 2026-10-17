#!/usr/bin/env python3
import socket
import time
for attempt in range(100):
    try:
        with socket.create_connection(("127.0.0.1", 6380), timeout=1) as client:
            client.sendall(b"*1\r\n$4\r\nPING\r\n")
            assert client.recv(7) == b"+PONG\r\n"
            break
    except OSError:
        time.sleep(.1)
else:
    raise RuntimeError("Docker server did not become ready")
