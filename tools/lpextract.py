#!/usr/bin/env python3
"""Pull one logical partition (e.g. vendor) out of an Android super image.

    lpextract.py SUPER.(s)img NAME OUT.img

SUPER may be an Android sparse image (AOSP libsparse) or raw. The partition is
located through the LP metadata (AOSP liblp: geometry at 4 KiB, primary
metadata after the geometry and its backup) and only its extents are copied,
so the whole super image is never expanded on disk.
"""
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


class Sparse:
    """Random-access reads from a sparse image without unsparsing it."""

    def __init__(self, f):
        self.f = f
        magic, _maj, _min, fhs, chs, self.bs, self.blocks, nchunks, _ = struct.unpack('<IHHHHIIII', f.read(28))
        assert magic == SPARSE_MAGIC
        f.seek(fhs)
        self.chunks = []        # (first_block, nblocks, kind, file_offset or fill bytes)
        blk = 0
        for _ in range(nchunks):
            kind, _r, n, total = struct.unpack('<HHII', f.read(12))
            if chs > 12:
                f.seek(chs - 12, 1)
            data = total - chs
            if kind == RAW:
                self.chunks.append((blk, n, RAW, f.tell()))
            elif kind == FILL:
                self.chunks.append((blk, n, FILL, f.read(4)))
                data -= 4
            elif kind == DONT_CARE:
                self.chunks.append((blk, n, DONT_CARE, None))
            if kind != FILL:
                f.seek(data, 1)
            if kind != CRC:
                blk += n
        self.size = self.blocks * self.bs

    def read(self, off, n):
        out = bytearray()
        while n:
            blk = off // self.bs
            c = next(c for c in self.chunks if c[0] <= blk < c[0] + c[1])
            within = off - c[0] * self.bs
            take = min(n, c[1] * self.bs - within)
            if c[2] == RAW:
                self.f.seek(c[3] + within)
                out += self.f.read(take)
            elif c[2] == FILL:
                pat = c[3] * (take // 4 + 2)
                s = within % 4
                out += pat[s:s + take]
            else:
                out += bytes(take)
            off += take
            n -= take
        return bytes(out)


class Raw:
    def __init__(self, f):
        self.f = f

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)


def main():
    src, name, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    f = open(src, 'rb')
    sparse = struct.unpack('<I', f.read(4))[0] == SPARSE_MAGIC
    f.seek(0)
    img = Sparse(f) if sparse else Raw(f)

    geo = img.read(4096, 52)
    gmagic, _gsz = struct.unpack('<II', geo[:8])
    assert gmagic == 0x616c4467, 'no LP geometry'
    meta_max, _slots, _lbs = struct.unpack('<III', geo[40:52])
    hdr_off = 4096 + 4096 * 2          # reserved + geometry + backup geometry
    h = img.read(hdr_off, 256)
    magic, maj, mnr, hsize = struct.unpack('<IHHI', h[:12])
    assert magic == 0x414C5030, 'no LP metadata header'
    tables_size = struct.unpack('<I', h[44:48])[0]
    p_off, p_n, p_sz, e_off, e_n, e_sz = struct.unpack('<IIIIII', h[80:104])
    tables = img.read(hdr_off + hsize, tables_size)
    parts = []
    for i in range(p_n):
        ent = tables[p_off + i * p_sz: p_off + (i + 1) * p_sz]
        pname = ent[:36].split(b'\0')[0].decode()
        _attr, first, num, _grp = struct.unpack('<IIII', ent[36:52])
        parts.append((pname, first, num))
    extents = []
    for i in range(e_n):
        ent = tables[e_off + i * e_sz: e_off + (i + 1) * e_sz]
        nsec, ttype, tdata, _src = struct.unpack('<QIQI', ent[:24])
        extents.append((nsec, ttype, tdata))
    print('LP v%d.%d partitions: %s' % (maj, mnr, ', '.join('%s(%d)' % (p[0], p[2]) for p in parts)))
    match = [p for p in parts if p[0] == name] or [p for p in parts if p[0] in (name + '_a',)]
    if not match:
        sys.exit('no partition %s' % name)
    _pn, first, num = match[0]
    total = 0
    with open(dst, 'wb') as o:
        for nsec, ttype, tdata in extents[first:first + num]:
            left, off = nsec * 512, tdata * 512
            while left:
                take = min(left, 64 << 20)
                o.write(img.read(off, take) if ttype == 0 else bytes(take))
                off += take
                left -= take
                total += take
    print('%s: %d MiB -> %s' % (match[0][0], total >> 20, dst))


if __name__ == '__main__':
    main()
