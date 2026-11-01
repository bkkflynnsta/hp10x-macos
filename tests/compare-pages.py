#!/usr/bin/env python3
# compare-pages - compare two decoded test pages (from make-testpage.py) pixel-wise
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: compare-pages.py reference.pbm candidate.pbm [dpi [weight]]   (exit 1 if out of tolerance)
#   weight: compare the text block's total ink rather than pixel by pixel (always at 1200 dpi)
import sys

TEXT_TOLERANCE = 0.5    # % of reference ink pixels allowed to differ in the text block
WEIGHT_TOLERANCE = 5.0  # 1200 dpi: % difference allowed in the text block's total ink
TONE_TOLERANCE = 3.0    # percentage points allowed per gray patch
MARGIN_PT = 12
DPI = int(sys.argv[3]) if len(sys.argv) > 3 else 600
BY_WEIGHT = DPI != 600 or sys.argv[4:5] == ["weight"]


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
    # A step of 3 shares no factor with the 8- or 16-pixel screen, so every phase is sampled.
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
    inset = 40 * DPI // 600
    top, bottom = px(792 - 690) + inset, px(792 - 600) - inset
    for i in range(11):
        x0, x1 = px(54 + i * 46) + inset, px(54 + i * 46 + 44) - inset
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
    if not BY_WEIGHT:
        diff = sum(bin(x ^ y).count("1") for x, y in zip(a[lo:hi], b[lo:hi]))
        pct = 100 * diff / max(ink, 1)
        print("%s text: %.2f%% of %d ink pixels differ" % ("ok  " if pct <= TEXT_TOLERANCE else "FAIL", pct, ink))
        ok &= pct <= TEXT_TOLERANCE
    else:
        # HP doubles its 600 dpi rendering, where ours is rendered at full resolution, and
        # its toner-save screen has rounder holes, so pixels differ by design there.
        # Compare stroke weight instead.
        pct = 100 * (sum(bin(v).count("1") for v in b[lo:hi]) - ink) / max(ink, 1)
        print("%s text: ink weight %+.1f%% of %d pixels" % ("ok  " if abs(pct) <= WEIGHT_TOLERANCE else "FAIL", pct, ink))
        ok &= abs(pct) <= WEIGHT_TOLERANCE
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
