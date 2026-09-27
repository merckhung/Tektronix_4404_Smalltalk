#!/usr/bin/env python3
"""Convert a binary PBM (P4) display dump from hal_trace into a 1-bit PNG.

    tools/pbm2png.py in.pbm out.png

Standard library only (zlib + struct), so no imaging package is needed.
"""

import struct
import sys
import zlib


def read_pbm(path):
    data = open(path, "rb").read()
    magic, dims, pixels = data.split(b"\n", 2)
    if magic != b"P4":
        sys.exit("%s: not a binary PBM" % path)
    width, height = map(int, dims.split())
    return width, height, pixels


def chunk(kind, body):
    crc = zlib.crc32(kind + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    width, height, pixels = read_pbm(src)
    stride = (width + 7) // 8
    # PBM: 1 = black. 1-bit greyscale PNG: 0 = black.
    rows = b"".join(b"\x00" + bytes(255 - b for b in pixels[y * stride:(y + 1) * stride])
                    for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)
    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
