#!/bin/bash
# compare-with-hp.sh - render test pages with HP's Samsung filter and ours, decode both,
# and compare text pixels and gray-ramp tones. Skips if HP's driver isn't installed.
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HPF=/Library/Printers/Samsung/UPD/Filters/rastertosec
HPPPD=${HPPPD:-$(ls /etc/cups/ppd/*.ppd 2>/dev/null | xargs grep -l '^\*ModelName: *"Samsung M2020' 2>/dev/null | head -1 || true)}

if [[ ! -x $HPF || -z $HPPPD ]]; then
    echo "skip: HP Samsung driver or an M2020 queue not installed"
    exit 0
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

python3 -I "$ROOT/tests/make-testpage.py" "$T/page.pdf"
cupsfilter -p "$HPPPD" -m application/vnd.cups-raster "$T/page.pdf" > "$T/page.ras" 2>/dev/null
PPD=$HPPPD "$HPF" 1 test test 1 "" "$T/page.ras" > "$T/hp.prn" 2>/dev/null
"$ROOT/build/rastertohp10x" 1 test test 1 "" "$T/page.ras" > "$T/ours.prn" 2>/dev/null

"$ROOT/build/spldecode" "$T/hp.prn" "$T/hp" > /dev/null
"$ROOT/build/spldecode" "$T/ours.prn" "$T/ours" > "$T/ours.txt"
grep -q 'end of document' "$T/ours.txt"

python3 -I "$ROOT/tests/compare-pages.py" "$T/hp-1.pbm" "$T/ours-1.pbm"
