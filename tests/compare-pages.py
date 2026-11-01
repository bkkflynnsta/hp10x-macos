#!/usr/bin/env python3
# compare-pages - compare two decoded test pages (from make-testpage.py) pixel-wise
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: compare-pages.py reference.pbm candidate.pbm   (exit 1 if out of tolerance)
import sys

TEXT_TOLERANCE = 0.5    # % of reference ink pixels allowed to differ in the text block
TONE_TOLERANCE = 3.0    # percentage points allowed per gray patch
MARGIN_PT, DPI = 12, 600


def load(path):
    data = open(path, "rb").read()
    magic, dims, bits = data.split(b"\n", 2)
    w, h = map(int, dims.split())
    return w, h, bits


def px(pt):
    return int((pt - MARGIN_PT) * DPI / 72)


def coverage(img, x0, y0, x1, y1):
    w, _, bits = img
    rb = (w + 7) // 8
    ink = total = 0
    for y in range(y0, y1, 3):
        for x in range(x0, x1, 3):
            ink += bits[y * rb + x // 8] >> (7 - x % 8) & 1
            total += 1
    return 100 * ink / total


def main():
    ref, cand = load(sys.argv[1]), load(sys.argv[2])
    ok = True
    if ref[:2] != cand[:2]:
        print("FAIL page size differs: %sx%s vs %sx%s" % (ref[0], ref[1], cand[0], cand[1]))
        return 1

    worst = 0.0
    top, bottom = px(792 - 690) + 40, px(792 - 600) - 40
    for i in range(11):
        x0, x1 = px(54 + i * 46) + 40, px(54 + i * 46 + 44) - 40
        a, b = coverage(ref, x0, top, x1, bottom), coverage(cand, x0, top, x1, bottom)
        worst = max(worst, abs(a - b))
    print("%s tone: worst patch difference %.1f points" % ("ok  " if worst <= TONE_TOLERANCE else "FAIL", worst))
    ok &= worst <= TONE_TOLERANCE

    w, _, a = ref
    b = cand[2]
    rb = (w + 7) // 8
    y0, y1 = px(792 - 575), px(792 - 470)
    lo, hi = y0 * rb, y1 * rb
    ink = sum(bin(v).count("1") for v in a[lo:hi])
    diff = sum(bin(x ^ y).count("1") for x, y in zip(a[lo:hi], b[lo:hi]))
    pct = 100 * diff / max(ink, 1)
    print("%s text: %.2f%% of %d ink pixels differ" % ("ok  " if pct <= TEXT_TOLERANCE else "FAIL", pct, ink))
    ok &= pct <= TEXT_TOLERANCE
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
