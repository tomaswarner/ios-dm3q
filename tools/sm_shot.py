#!/usr/bin/env python3
"""Screenshot of the DM3Q panel over USB: runs sm_shot through sweetrun, writes a PNG.

  tools/sm_shot.py OUT.png [scale]    (scale 3 = 480x1029 of the 1440x3088 panel)
"""
import base64, os, subprocess, sys, zlib, struct

here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1]
scale = sys.argv[2] if len(sys.argv) > 2 else "3"
binary = os.path.join(here, "sweettest", "sm_shot")
r = subprocess.run([sys.executable, os.path.join(here, "sweetrun.py"), "-f", binary, "sm_shot", scale],
                   capture_output=True, text=True, timeout=120)
lines = r.stdout.splitlines()
try:
    head = next(i for i, l in enumerate(lines) if l.startswith("SHOT ") and l != "SHOT END")
    end = lines.index("SHOT END")
except (StopIteration, ValueError):
    sys.exit("no screenshot:\n" + r.stdout[-2000:] + r.stderr[-2000:])
_, w, h = lines[head].split()
w, h = int(w), int(h)
raw = base64.b64decode("".join(lines[head + 1:end]))
rows = b"".join(b"\0" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
    chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print(f"{out}: {w}x{h}")
