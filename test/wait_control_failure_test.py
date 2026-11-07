#!/usr/bin/env python3
"""Exercise the real TCP protocol's disconnect and server-death boundaries."""
import select
import socket
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

MAGIC = 0x52524C5741495432


def request(sock, *values):
    sock.sendall(struct.pack('!16Q', MAGIC, *values, *([0] * (15 - len(values)))))
    data = b''
    while len(data) < 128:
        part = sock.recv(128 - len(data))
        if not part:
            raise EOFError('coordinator closed')
        data += part
    reply = struct.unpack('!16Q', data)
    assert reply[0] == MAGIC
    return reply


def main():
    with socket.socket() as bind:
        bind.bind(('127.0.0.1', 0))
        port = bind.getsockname()[1]
    server = subprocess.Popen([sys.argv[1], str(port)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    clients = []
    try:
        assert select.select([server.stdout], [], [], 5)[0]
        assert b'listening' in server.stdout.readline()
        for _ in range(2):
            s = socket.create_connection(('127.0.0.1', port), timeout=3)
            s.settimeout(3)
            clients.append(s)
        a, b = clients
        old = request(a, 1, 2, 0, 2_000_000_000)[2]
        young = request(b, 1, 2, 0, 2_000_000_000)[2]
        assert request(a, 2, old, 3, 0, 7, 7, 1, 1, 1, 1)[1] == 0
        with ThreadPoolExecutor(max_workers=1) as pool:
            blocked = pool.submit(request, b, 2, young, 3, 0, 7, 7, 1, 1, 1, 1)
            a.shutdown(socket.SHUT_RDWR)
            a.close()
            # An active disconnect must wake the waiter with Error, never grant
            # the lock by deleting metadata ahead of physical RDMA recovery.
            assert blocked.result(timeout=4)[1] == 4
        assert request(b, 4, young)[1] == 4
        assert request(b, 5, young)[1] == 0
        assert request(b, 1, 2, 0, 2_000_000_000)[1] == 4
        server.terminate()
        server.wait(timeout=5)
        try:
            request(b, 3, young)
        except (EOFError, OSError):
            pass
        else:
            raise AssertionError('server death returned a normal result')
        print('TCP disconnect fails closed; waiter wakes; server death terminates I/O: OK')
    finally:
        for s in clients:
            s.close()
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=5)


if __name__ == '__main__':
    main()
