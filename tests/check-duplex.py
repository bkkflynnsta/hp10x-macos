#!/usr/bin/env python3
# check-duplex - check a decoded manual-duplex job against its simplex pages
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: check-duplex.py simplex-prefix duplex-prefix n raster-width raster-height expected
#   expected: output pages in order, e.g. "4r 2r 1 3" (r = turned 180°, B = blank)
import sys


def load(path):
    data = open(path, "rb").read()
    magic, dims, bits = data.split(b"\n", 2)
    w, h = map(int, dims.split())
    return w, h, bits


def turned(img, rw, rh):
    """Turn the raster area (rw x rh at the top left) by 180°, as the filter does."""
    w, h, bits = img
    rb = (w + 7) // 8
    out = bytearray(len(bits))
    for y in range(rh):
        row = int.from_bytes(bits[y * rb:(y + 1) * rb], "big") >> (rb * 8 - rw)
        rev = int(bin(row)[2:].zfill(rw)[::-1], 2) << (rb * 8 - rw)
        out[(rh - 1 - y) * rb:(rh - y) * rb] = rev.to_bytes(rb, "big")
    return bytes(out)


def main():
    simplex, duplex, n, rw, rh, expected = sys.argv[1:7]
    pages = {str(k): load("%s-%d.pbm" % (simplex, k)) for k in range(1, int(n) + 1)}
    got = []
    for i in range(1, len(expected.split()) + 1):
        bits = load("%s-%d.pbm" % (duplex, i))[2]
        name = "?"
        if not any(bits):
            name = "B"
        for k, img in pages.items():
            if bits == img[2]:
                name = k
            elif bits == turned(img, int(rw), int(rh)):
                name = k + "r"
        got.append(name)
    ok = " ".join(got) == expected
    print("%s duplex pages: %s (want %s)" % ("ok  " if ok else "FAIL", " ".join(got), expected))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
