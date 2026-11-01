#!/bin/bash
# duplex-filter.sh - run the filter's two manual-duplex steps on numbered pages and check
# the page order and which pages are turned.
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

python3 -I "$ROOT/tests/make-numbered.py" "$T/p.pdf" 5
cupsfilter -p "$ROOT/ppd/HP-Laser-10x.ppd" -m application/vnd.cups-raster "$T/p.pdf" > "$T/p.ras" 2>/dev/null
# Raster width and height (cupsWidth, cupsHeight), for turning pages in the check.
read -r RW RH < <(python3 -I -c '
import struct, sys
h = open(sys.argv[1], "rb").read(1800)[4:]
e = ">" if struct.unpack(">I", h[276:280])[0] in (600, 1200) else "<"   # HWResolution[0]
print(*struct.unpack(e + "2I", h[372:380]))' "$T/p.ras")

"$ROOT/build/rastertohp10x" 1 t t 1 "" "$T/p.ras" > "$T/s.prn" 2>/dev/null
"$ROOT/build/spldecode" "$T/s.prn" "$T/s" > /dev/null
for opt in LongEdgeBacks LongEdgeFronts ShortEdgeBacks ShortEdgeFronts; do
    "$ROOT/build/rastertohp10x" 1 t t 1 "ManualDuplex=$opt" "$T/p.ras" > "$T/$opt.prn" 2>/dev/null
    "$ROOT/build/spldecode" "$T/$opt.prn" "$T/$opt" > /dev/null
done
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/LongEdgeBacks" 5 "$RW" "$RH" "B 4r 2r"
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/LongEdgeFronts" 5 "$RW" "$RH" "1 3 5"
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/ShortEdgeBacks" 5 "$RW" "$RH" "B 4 2"
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/ShortEdgeFronts" 5 "$RW" "$RH" "1 3 5"

# Two copies of a 3-page document: each copy gets its own two sheets.
python3 -I "$ROOT/tests/make-numbered.py" "$T/p3.pdf" 3
cupsfilter -p "$ROOT/ppd/HP-Laser-10x.ppd" -m application/vnd.cups-raster -n 2 "$T/p3.pdf" > "$T/p3.ras" 2>/dev/null
for opt in LongEdgeBacks LongEdgeFronts; do
    "$ROOT/build/rastertohp10x" 1 t t 2 "ManualDuplex=$opt" "$T/p3.ras" > "$T/c$opt.prn" 2>/dev/null
    "$ROOT/build/spldecode" "$T/c$opt.prn" "$T/c$opt" > /dev/null
done
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/cLongEdgeBacks" 3 "$RW" "$RH" "B 2r B 2r"
python3 -I "$ROOT/tests/check-duplex.py" "$T/s" "$T/cLongEdgeFronts" 3 "$RW" "$RH" "1 3 1 3"
