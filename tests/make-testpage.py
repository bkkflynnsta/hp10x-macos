#!/usr/bin/env python3
# make-testpage - write a one-page PDF with a gray ramp, text sizes, fine lines and a border
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: make-testpage.py out.pdf [title] [Letter|A4]
import sys

title = sys.argv[2] if len(sys.argv) > 2 else "HP Laser 103/107/108 driver test"
W, H = {"Letter": (612, 792), "A4": (595, 842)}[sys.argv[3] if len(sys.argv) > 3 else "Letter"]
dy = H - 792    # layout is measured from the top of a Letter page

ops = ["BT /F1 22 Tf 54 %d Td (%s) Tj ET" % (730 + dy, title.replace("(", "[").replace(")", "]"))]
# Gray ramp: 11 patches from white to black
for i in range(11):
    g = 1 - i / 10
    ops.append("%.2f g %d %d 44 90 re f" % (g, 54 + i * 46, 600 + dy))
ops.append("0 g")
y = 560 + dy
for size in (24, 14, 10, 8, 6):
    ops.append("BT /F2 %d Tf 54 %d Td (%dpt: The quick brown fox jumps over the lazy dog 0123456789) Tj ET" % (size, y, size))
    y -= size + 14
# Fine lines, 0.25pt to 2pt
x = 54
for w in (0.25, 0.5, 1, 2):
    ops.append("%.2f w %d %d m %d %d l S" % (w, x, 300 + dy, x, 400 + dy))
    x += 30
# Page border just inside the 12pt imageable margin, plus corner marks
ops.append("0.5 w 14 14 %d %d re S" % (W - 28, H - 28))
ops.append("BT /F1 9 Tf 20 20 Td (bottom-left) Tj ET BT /F1 9 Tf %d %d Td (top-right) Tj ET" % (W - 82, H - 22))

content = "\n".join(ops).encode("latin-1")
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents 4 0 R "
    b"/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> >>" % (W, H),
    b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>",
]
out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, body in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
open(sys.argv[1], "wb").write(out)
