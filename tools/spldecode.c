/*
 * spldecode - decode an SPL/QPDL v3 (algorithm 0x11) stream into PBM page images
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Usage: spldecode file.prn [outprefix]
 * Prints the PJL header and one line per page/band, verifies checksums, and writes
 * <outprefix>-<n>.pbm for each page (1 = black, as in the source raster).
 */
#include "../src/algo0x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int fail(const char *msg, size_t off)
{
    fprintf(stderr, "spldecode: %s at offset %zu\n", msg, off);
    return 1;
}

int main(int argc, char **argv)
{
    const char *prefix = argc > 2 ? argv[2] : NULL;
    const char *marker = "ENTER LANGUAGE=QPDL";
    FILE *f;
    uint8_t *d, *bmp = NULL;
    size_t n, p, cap = 1 << 20;
    int page = 0, errors = 0, width = 0, height = 0, row_bytes = 0;
    char *m;

    if (argc < 2) {
        fprintf(stderr, "usage: spldecode file.prn [outprefix]\n");
        return 2;
    }
    if (!(f = fopen(argv[1], "rb")))
        return fail("cannot open input", 0);
    d = malloc(cap);
    for (n = 0; (p = fread(d + n, 1, cap - n, f)) > 0; ) {
        n += p;
        if (n == cap)
            d = realloc(d, cap *= 2);
    }
    fclose(f);

    if (!(m = memmem(d, n, marker, strlen(marker))))
        return fail("no ENTER LANGUAGE=QPDL", 0);
    p = (size_t)((uint8_t *)m - d) + strlen(marker);
    fwrite(d, 1, p, stdout);
    putchar('\n');
    while (p < n && (d[p] == '\r' || d[p] == '\n'))
        p++;

    while (p < n) {
        uint8_t t = d[p];

        if (t == 0x00) {                            /* page header */
            const uint8_t *h = d + p;
            if (p + 17 > n)
                return fail("truncated page header", p);
            int yres = h[1] * 100, xres = h[0x10] * 100;
            int w300 = (h[5] << 8) | h[6], h300 = (h[7] << 8) | h[8];
            /* The size field is 1/300 inch scaled by 600/dpi: 1/150 inch at 1200 dpi. */
            width = (int)((long)w300 * xres * xres / 180000);
            height = (int)((long)h300 * yres * yres / 180000);
            row_bytes = (width + 7) / 8;
            free(bmp);
            bmp = calloc((size_t)row_bytes, (size_t)height);
            page++;
            printf("page %d: %dx%d dpi copies=%d paper=%d size=%dx%d/300in source=%d "
                   "duplex=%d tumble=%d ver=%d bytes[a,d,f]=%02x,%02x,%02x\n",
                   page, xres, yres, (h[2] << 8) | h[3], h[4], w300, h300, h[9],
                   h[0xb], h[0xc], h[0xe], h[0xa], h[0xd], h[0xf]);
            p += 17;
        } else if (t == 0x0C) {                     /* band */
            const uint8_t *h = d + p;
            if (p + 11 > n || !bmp)
                return fail("band outside a page", p);
            int bn = h[1], bw = (h[2] << 8) | h[3], bh = (h[4] << 8) | h[5], algo = h[6];
            uint32_t size = be32(h + 7), sum = 0;
            const uint8_t *data = h + 11;
            if (p + 11 + size > n || size < 8)
                return fail("truncated band", p);
            for (uint32_t i = 0; i < size - 4; i++)
                sum += data[i];
            uint32_t ck = be32(data + size - 4);
            int ok_sig = data[0] == 0xEF && data[1] == 0xCD && data[2] == 0xAB && data[3] == 0x09;
            size_t bb = (size_t)bw / 8, raw_cap = bb * (size_t)bh;
            uint8_t *raw = malloc(raw_cap);
            long got = algo == 0x11 ? a11_decompress(data + 4, size - 8, raw, raw_cap) : -1;
            printf("  band %3d: %dx%d algo=%#x size=%u checksum %s%s%s\n", bn, bw, bh, algo,
                   size, ck == sum ? "ok" : "BAD", ok_sig ? "" : " signature BAD",
                   got == (long)raw_cap ? "" : " DECOMPRESS FAILED");
            if (ck != sum || !ok_sig || got != (long)raw_cap)
                errors++;
            else
                /* Column-major and inverted -> row-major, 1 = black. */
                for (size_t x = 0; x < bb && x < (size_t)row_bytes; x++)
                    for (int y = 0; y < bh; y++) {
                        long row = (long)bn * bh + y;
                        if (row < height)
                            bmp[row * row_bytes + x] = (uint8_t)~raw[x * bh + y];
                    }
            free(raw);
            p += 11 + size;
        } else if (t == 0x01) {                     /* page footer */
            if (p + 3 > n)
                return fail("truncated page footer", p);
            printf("  end of page (copies=%d)\n", (d[p + 1] << 8) | d[p + 2]);
            if (prefix && bmp) {
                char path[1024];
                snprintf(path, sizeof(path), "%s-%d.pbm", prefix, page);
                FILE *o = fopen(path, "wb");
                if (o) {
                    fprintf(o, "P4\n%d %d\n", width, height);
                    fwrite(bmp, (size_t)row_bytes, (size_t)height, o);
                    fclose(o);
                }
            }
            p += 3;
        } else if (t == 0x09) {                     /* end of document */
            printf("end of document\n");
            p++;
        } else if (t == 0x1B) {                     /* UEL / trailing PJL */
            printf("trailer: %zu bytes\n", n - p);
            break;
        } else {
            return fail("unknown record", p);
        }
    }
    free(bmp);
    free(d);
    if (errors)
        fprintf(stderr, "spldecode: %d bad band(s)\n", errors);
    return errors ? 1 : 0;
}
