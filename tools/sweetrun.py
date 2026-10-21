#!/usr/bin/env python3
"""Mac side of sweetrun (tools/sweettest/sweetrun.c): upload files to the phone's
/usr/local/sweettest/ over the USB Ethernet link and run one, streaming its output.

  sweetrun.py [-f FILE]... NAME [ARG...]

Each -f FILE is uploaded under its base name first; NAME is then run from
/usr/local/sweettest/. The token is read from ~/.sweetrun-token (the one baked
into the root image by tools/add-sweetrun.sh). Exit code: the test's.
"""
import os
import socket
import sys

HOST, PORT = "192.168.7.2", 5141


def main():
    args = sys.argv[1:]
    files = []
    while len(args) >= 2 and args[0] == "-f":
        files.append(args[1])
        args = args[2:]
    if not args:
        sys.exit(__doc__)
    token = open(os.path.expanduser("~/.sweetrun-token")).read().strip()
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.settimeout(None)
    s.sendall(b"TOKEN %s\n" % token.encode())
    for path in files:
        data = open(path, "rb").read()
        s.sendall(b"FILE %s %d\n" % (os.path.basename(path).encode(), len(data)))
        s.sendall(data)
    s.sendall(("RUN " + " ".join(args) + "\n").encode())
    status = None
    tail = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
        tail = (tail + chunk)[-256:]
    for line in tail.decode(errors="replace").splitlines():
        if line.startswith("SWEETRUN EXIT "):
            status = int(line.split()[2], 16)
    if status is None:
        sys.exit(1)
    sys.exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status))


if __name__ == "__main__":
    main()
