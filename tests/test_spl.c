/*
 * test_spl - band widths and manual-duplex page order, checked against HP's driver 3.93
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../src/algo0x11.h"
#include "../src/spl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *fmt, const char *what, const char *got, const char *want)
{
    printf(ok ? "ok   " : "FAIL ");
    printf(fmt, what, got, want);
    printf("\n");
    if (!ok)
        failures++;
}

/* HP's band widths in bytes, per paper at 600 and 1200 dpi, and for custom widths. */
static void test_band_bytes(void)
{
    static const struct { const char *name; double pts; int b600, b1200; } hp[] = {
        { "Letter", 612, 640, 1248 }, { "Legal", 612, 640, 1248 }, { "A4", 595, 608, 1216 },
        { "Executive", 522, 544, 1056 }, { "Env10", 297, 288, 576 }, { "EnvMonarch", 279, 288, 544 },
        { "EnvC5", 459, 480, 928 }, { "EnvDL", 312, 320, 608 }, { "B5", 516, 544, 1056 },
        { "ISOB5", 499, 512, 992 }, { "4x6", 288, 288, 576 }, { "A5", 420, 416, 832 },
        { "Folio", 612, 640, 1248 }, { "Oficio", 612, 640, 1248 },
        { "custom 216pt", 216, 224, 0 }, { "custom 250pt", 250, 256, 0 },
        { "custom 300pt", 300, 288, 0 }, { "custom 400pt", 400, 416, 0 },
        { "custom 450pt", 450, 448, 0 }, { "custom 500pt", 500, 512, 0 },
        { "custom 550pt", 550, 576, 0 }, { "custom 600pt", 600, 608, 0 },
    };
    int bad = 0;

    for (size_t i = 0; i < sizeof(hp) / sizeof(hp[0]); i++) {
        int a = spl_band_bytes(hp[i].pts, 600), b = spl_band_bytes(hp[i].pts, 1200);
        if (a != hp[i].b600 || (hp[i].b1200 && b != hp[i].b1200)) {
            printf("FAIL band width %s: %d/%d, HP %d/%d\n", hp[i].name, a, b, hp[i].b600, hp[i].b1200);
            bad++;
        }
    }
    if (!bad)
        printf("ok   band widths match HP for %zu papers\n", sizeof(hp) / sizeof(hp[0]));
    failures += bad;
}

/*
 * Write an n-page manual-duplex job for `step` and describe the pages in the order they
 * come out: the document page (from the page's content), "r" if turned 180°, or "B" for
 * a blank page. With copies > 1 the n pages are those copies, collated (1 2 3 1 2 3) or
 * not (1 1 2 2 3 3). Pages carrying the printer's own duplex flags are marked "!".
 */
static void duplex_order(int duplex, int step, int n, int copies, int uncollated, char *out, size_t cap)
{
    enum { ROW = 80, BAND = 96 };
    char *buf = NULL;
    size_t size = 0, p, len = 0;
    FILE *f = open_memstream(&buf, &size);
    spl_writer *w = spl_open(f);
    spl_job_t job = { "t", "t", 600, duplex, step, copies };
    uint8_t row[ROW], raw[BAND * SPL_BAND_HEIGHT];

    spl_job_begin(w, &job);
    for (int k = 0; k < n; k++) {
        spl_page_t pg = { 600, 600, 0, 2550, 3300, 1, 1, BAND };
        int doc = uncollated ? k / copies : k % (n / copies);

        memset(row, 0, sizeof(row));
        row[doc] = 0xFF;                    /* a black column marks the document page */
        spl_page_begin(w, &pg, ROW * 8);
        for (int y = 0; y < 200; y++)
            spl_page_row(w, row);
        spl_page_end(w);
    }
    spl_job_end(w);
    spl_close(w);
    fclose(f);

    out[0] = '\0';
    p = (size_t)(strstr(buf, "QPDL\r\n") - buf) + 6;
    while (p < size && buf[p] != 0x09) {
        const uint8_t *h = (const uint8_t *)buf + p;
        char name[8] = "B";

        p += 17;
        while ((uint8_t)buf[p] == 0x0C) {   /* band: 11-byte header, signature, data, checksum */
            const uint8_t *b = (const uint8_t *)buf + p;
            size_t bs = ((size_t)b[7] << 24) | ((size_t)b[8] << 16) | ((size_t)b[9] << 8) | b[10];
            if (b[1] == 0 && a11_decompress(b + 15, bs - 8, raw, sizeof(raw)) == (long)sizeof(raw))
                for (int x = 0; x < ROW; x++)
                    if (raw[x * SPL_BAND_HEIGHT] == 0x00)   /* column-major, inverted */
                        snprintf(name, sizeof(name), x < ROW / 2 ? "%d" : "%dr",
                                 (x < ROW / 2 ? x : ROW - 1 - x) + 1);
            p += 11 + bs;
        }
        p += 3;                             /* footer */
        len += (size_t)snprintf(out + len, cap - len, "%s%s%s", len ? " " : "", name,
                                h[9] != 1 || h[0xb] || h[0xc] ? "!" : "");
    }
    free(buf);
}

static void test_duplex(void)
{
    /* Backs are HP's pass 1 (even pages last-first, turned for long edge), fronts pass 2. */
    static const struct { int duplex, n, copies, uncollated; const char *backs, *fronts; } cases[] = {
        { SPL_DUPLEX_LONG, 1, 1, 0, "", "1" },
        { SPL_DUPLEX_LONG, 2, 1, 0, "2r", "1" },
        { SPL_DUPLEX_LONG, 3, 1, 0, "B 2r", "1 3" },
        { SPL_DUPLEX_LONG, 4, 1, 0, "4r 2r", "1 3" },
        { SPL_DUPLEX_LONG, 5, 1, 0, "B 4r 2r", "1 3 5" },
        { SPL_DUPLEX_SHORT, 5, 1, 0, "B 4 2", "1 3 5" },
        /* Copies: each gets its own sheets, whichever order they arrive in. */
        { SPL_DUPLEX_LONG, 6, 2, 0, "B 2r B 2r", "1 3 1 3" },
        { SPL_DUPLEX_LONG, 6, 2, 1, "B 2r B 2r", "1 3 1 3" },
        { SPL_DUPLEX_LONG, 4, 2, 0, "2r 2r", "1 1" },
        { SPL_DUPLEX_LONG, 3, 3, 0, "", "1 1 1" },
    };
    char backs[256], fronts[256], what[64];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        duplex_order(cases[i].duplex, 1, cases[i].n, cases[i].copies, cases[i].uncollated,
                     backs, sizeof(backs));
        duplex_order(cases[i].duplex, 2, cases[i].n, cases[i].copies, cases[i].uncollated,
                     fronts, sizeof(fronts));
        snprintf(what, sizeof(what), "%s, %d pages%s",
                 cases[i].duplex == SPL_DUPLEX_LONG ? "long edge" : "short edge", cases[i].n,
                 cases[i].copies < 2 ? "" : cases[i].uncollated ? " (uncollated copies)" : " (copies)");
        char got[520], want[520];
        snprintf(got, sizeof(got), "backs [%s] fronts [%s]", backs, fronts);
        snprintf(want, sizeof(want), "backs [%s] fronts [%s]", cases[i].backs, cases[i].fronts);
        check(!strcmp(got, want), "duplex %-36s %s (want %s)", what, got, want);
    }
    duplex_order(SPL_DUPLEX_NONE, 0, 3, 1, 0, fronts, sizeof(fronts));
    check(!strcmp(fronts, "1 2 3"), "duplex %-36s %s (want %s)", "off, 3 pages", fronts, "1 2 3");
}

int main(void)
{
    test_band_bytes();
    test_duplex();
    return failures ? 1 : 0;
}
