#!/usr/bin/env python3
"""List the functions of a .metallib (MTLB container) and their stage.

    metallib-ls.py FILE.metallib

Layout per YuAo/MetalLibraryArchive: 'MTLB' header with (offset, size) pairs
for the function list, public metadata, private metadata and bitcode; the
function list is a u32 count of entries, each a u32 length followed by
tagged fields (4-char tag, u16 size, data) ending in 'ENDT'. NAME = name
(NUL-terminated), TYPE = u8 function type (0 vertex, 1 fragment, 2 kernel, ...).
"""
import struct
import sys

TYPES = {0: 'vertex', 1: 'fragment', 2: 'kernel', 3: 'unqualified', 4: 'visible',
         5: 'extern', 6: 'intersection', 7: 'mesh', 8: 'object'}


def functions(data):
    assert data[:4] == b'MTLB', 'not a metallib'
    flist_off, _flist_size = struct.unpack_from('<QQ', data, 0x18)
    count = struct.unpack_from('<I', data, flist_off)[0]
    pos = flist_off + 4
    for _ in range(count):
        length = struct.unpack_from('<I', data, pos)[0]
        p, end = pos + 4, pos + length
        name, ftype = None, None
        while p < end:
            tag = data[p:p + 4]
            if tag == b'ENDT':
                break
            size = struct.unpack_from('<H', data, p + 4)[0]
            body = data[p + 6:p + 6 + size]
            if tag == b'NAME':
                name = body.split(b'\0')[0].decode(errors='replace')
            elif tag == b'TYPE':
                ftype = body[0]
            p += 6 + size
        yield name, TYPES.get(ftype, str(ftype))
        pos = end


if __name__ == '__main__':
    for name, kind in functions(open(sys.argv[1], 'rb').read()):
        print(kind, name)
