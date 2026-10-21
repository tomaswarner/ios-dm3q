#!/usr/bin/env python3
"""Raw image -> Android sparse image (format of AOSP libsparse), for Samsung
Download Mode, which takes USERDATA as a sparse image.

    img2simg.py RAW.img OUT.simg [BLOCK=4096]

All-zero blocks become DONT_CARE chunks (not written: HFS ignores whatever the
free blocks hold), everything else RAW chunks of at most 64 MiB.
"""
import struct
import sys

MAGIC = 0xED26FF3A
CHUNK_RAW, CHUNK_DONT_CARE = 0xCAC1, 0xCAC3
MAX_RAW = 64 << 20


def main():
    src, dst = sys.argv[1], sys.argv[2]
    bs = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
    zero = bytes(bs)
    chunks = []             # (type, nblocks)
    total = 0
    with open(src, 'rb') as f:
        while True:
            b = f.read(bs)
            if not b:
                break
            if len(b) < bs:
                b += bytes(bs - len(b))
            kind = CHUNK_DONT_CARE if b == zero else CHUNK_RAW
            if chunks and chunks[-1][0] == kind and (kind == CHUNK_DONT_CARE or (chunks[-1][1] + 1) * bs <= MAX_RAW):
                chunks[-1][1] += 1
            else:
                chunks.append([kind, 1])
            total += 1
    raw_blocks = sum(n for k, n in chunks if k == CHUNK_RAW)
    print(f'{total} blocks, {len(chunks)} chunks, {raw_blocks * bs >> 20} MiB of data', file=sys.stderr)

    with open(src, 'rb') as f, open(dst, 'wb') as o:
        o.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, bs, total, len(chunks), 0))
        for kind, n in chunks:
            if kind == CHUNK_RAW:
                o.write(struct.pack('<HHII', kind, 0, n, 12 + n * bs))
                left = n * bs
                while left:
                    b = f.read(min(left, 8 << 20))
                    if len(b) < min(left, 8 << 20):
                        b += bytes(min(left, 8 << 20) - len(b))
                    o.write(b)
                    left -= len(b)
            else:
                o.write(struct.pack('<HHII', kind, 0, n, 12))
                f.seek(n * bs, 1)


if __name__ == '__main__':
    main()
