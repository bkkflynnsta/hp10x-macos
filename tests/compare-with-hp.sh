#!/bin/bash
# compare-with-hp.sh - render test pages with HP's Samsung filter and ours, decode both,
# and compare page headers, text pixels and gray-ramp tones at 600 and 1200 dpi.
# Skips if HP's driver isn't installed.
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

# Page header line from spldecode, e.g. "page 1: 600x600 dpi copies=1 paper=0 size=... ".
header() { "$ROOT/build/spldecode" "$1" | grep -m1 '^page'; }
band() { "$ROOT/build/spldecode" "$1" | grep -m1 'band' | awk '{print $3}'; }
same_header() {
    if [[ $(header "$2") == "$(header "$3")" && $(band "$2") == "$(band "$3")" ]]; then
        echo "ok   $1: page header and band width match"
    else
        echo "FAIL $1: HP: $(header "$2") $(band "$2")"
        echo "     $1: us: $(header "$3") $(band "$3")"
        return 1
    fi
}

python3 -I "$ROOT/tests/make-testpage.py" "$T/page.pdf"
# The PPD offers only 600 dpi (the 108a drops 1200 dpi pages); add 1200 to check the encoder.
sed 's#^\*Resolution 600dpi/.*#&\
*Resolution 1200dpi/1200 dpi: "<</HWResolution[1200 1200]>>setpagedevice"#' \
    "$ROOT/ppd/HP-Laser-10x.ppd" > "$T/ours.ppd"

# 600 dpi: both filters get the same raster.
cupsfilter -p "$HPPPD" -m application/vnd.cups-raster "$T/page.pdf" > "$T/page.ras" 2>/dev/null
PPD=$HPPPD "$HPF" 1 test test 1 "" "$T/page.ras" > "$T/hp.prn" 2>/dev/null
"$ROOT/build/rastertohp10x" 1 test test 1 "" "$T/page.ras" > "$T/ours.prn" 2>/dev/null

"$ROOT/build/spldecode" "$T/hp.prn" "$T/hp" > /dev/null
"$ROOT/build/spldecode" "$T/ours.prn" "$T/ours" > "$T/ours.txt"
grep -q 'end of document' "$T/ours.txt"

echo "600 dpi:"
same_header "Letter" "$T/hp.prn" "$T/ours.prn"
python3 -I "$ROOT/tests/compare-pages.py" "$T/hp-1.pbm" "$T/ours-1.pbm"

# 1200 dpi: HP's High Resolution upsamples a 600 dpi raster; ours renders at 1200.
# (Neither prints on the 108a, but the encoder is kept for the other models.)
cupsfilter -p "$HPPPD" -m application/vnd.cups-raster -o SECResolution=Best "$T/page.pdf" > "$T/hp1200.ras" 2>/dev/null
cupsfilter -p "$T/ours.ppd" -m application/vnd.cups-raster -o Resolution=1200dpi \
    "$T/page.pdf" > "$T/ours1200.ras" 2>/dev/null
PPD=$HPPPD "$HPF" 1 test test 1 "SECResolution=Best" "$T/hp1200.ras" > "$T/hp1200.prn" 2>/dev/null
"$ROOT/build/rastertohp10x" 1 test test 1 "" "$T/ours1200.ras" > "$T/ours1200.prn" 2>/dev/null
"$ROOT/build/spldecode" "$T/hp1200.prn" "$T/hp1200" > /dev/null
"$ROOT/build/spldecode" "$T/ours1200.prn" "$T/ours1200" > /dev/null

echo "1200 dpi:"
same_header "Letter" "$T/hp1200.prn" "$T/ours1200.prn"
python3 -I "$ROOT/tests/compare-pages.py" "$T/hp1200-1.pbm" "$T/ours1200-1.pbm" 1200

# Toner save at both resolutions.
for q in Normal Best; do
    dpi=$([[ $q == Best ]] && echo 1200 || echo 600)
    cupsfilter -p "$HPPPD" -m application/vnd.cups-raster -o SECResolution=$q -o id_SaveMode=On \
        "$T/page.pdf" > "$T/hpts.ras" 2>/dev/null
    cupsfilter -p "$T/ours.ppd" -m application/vnd.cups-raster -o Resolution=${dpi}dpi \
        "$T/page.pdf" > "$T/oursts.ras" 2>/dev/null
    PPD=$HPPPD "$HPF" 1 test test 1 "SECResolution=$q id_SaveMode=On" "$T/hpts.ras" > "$T/hpts.prn" 2>/dev/null
    "$ROOT/build/rastertohp10x" 1 test test 1 "TonerSave=True" "$T/oursts.ras" > "$T/oursts.prn" 2>/dev/null
    "$ROOT/build/spldecode" "$T/hpts.prn" "$T/hpts" > /dev/null
    "$ROOT/build/spldecode" "$T/oursts.prn" "$T/oursts" > /dev/null
    echo "toner save, $dpi dpi:"
    python3 -I "$ROOT/tests/compare-pages.py" "$T/hpts-1.pbm" "$T/oursts-1.pbm" $dpi weight
done

# Custom size, 600 dpi. (HP's 1200 dpi custom bands are narrower than the page.)
echo "custom size:"
python3 -I - "$T/custom.pdf" <<'EOF'
import sys
c = b"0 g 30 30 100 100 re f"
objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 250 400] /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(c) + c + b"\nendstream"]
out, offs = bytearray(b"%PDF-1.4\n"), []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
x = len(out)
out += b"xref\n0 5\n0000000000 65535 f \n" + b"".join(b"%010d 00000 n \n" % o for o in offs)
out += b"trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % x
open(sys.argv[1], "wb").write(out)
EOF
cupsfilter -p "$HPPPD" -m application/vnd.cups-raster "$T/custom.pdf" > "$T/hpc.ras" 2>/dev/null
cupsfilter -p "$ROOT/ppd/HP-Laser-10x.ppd" -m application/vnd.cups-raster -o PageSize=Custom.250x400 \
    "$T/custom.pdf" > "$T/oursc.ras" 2>/dev/null
PPD=$HPPPD "$HPF" 1 test test 1 "" "$T/hpc.ras" > "$T/hpc.prn" 2>/dev/null
"$ROOT/build/rastertohp10x" 1 test test 1 "" "$T/oursc.ras" > "$T/oursc.prn" 2>/dev/null
same_header "250x400pt" "$T/hpc.prn" "$T/oursc.prn"
