#!/usr/bin/env python3
# make-numbered - write an n-page Letter PDF, each page showing its number large and off-centre
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: make-numbered.py out.pdf n
import sys

n = int(sys.argv[2])
objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(b"%d 0 R" % (4 + 2 * i) for i in range(n)), n),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
for i in range(n):
    c = b"BT /F1 300 Tf 120 380 Td (%d) Tj ET" % (i + 1)
    objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents %d 0 R "
                b"/Resources << /Font << /F1 3 0 R >> >> >>" % (5 + 2 * i))
    objs.append(b"<< /Length %d >>\nstream\n" % len(c) + c + b"\nendstream")
out, offs = bytearray(b"%PDF-1.4\n"), []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1) + b"".join(b"%010d 00000 n \n" % o for o in offs)
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open(sys.argv[1], "wb").write(out)
