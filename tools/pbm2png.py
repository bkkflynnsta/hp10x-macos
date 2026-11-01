#!/usr/bin/env python3
# pbm2png - crop a P4 PBM and save it as a grayscale PNG (no third-party modules)
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: pbm2png.py in.pbm out.png [x y w h] [scale]
import struct
import sys
import zlib


def read_pbm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    assert fields[0] == b"P4", "not a binary PBM"
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:], (w + 7) // 8


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, bits, rb = read_pbm(src)
    x0, y0, cw, ch = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 0, w, h)
    scale = int(sys.argv[7]) if len(sys.argv) >= 8 else 1
    cw, ch = min(cw, w - x0), min(ch, h - y0)
    rows = []
    for y in range(y0, y0 + ch, scale):
        row = bytearray([0])
        for x in range(x0, x0 + cw, scale):
            black = bits[y * rb + x // 8] >> (7 - x % 8) & 1
            row.append(0 if black else 255)
        rows.append(bytes(row))
    ow, oh = len(rows[0]) - 1, len(rows)

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()
