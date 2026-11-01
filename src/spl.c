/*
 * spl.c - SPL/QPDL v3 writer for the HP Laser 103/107/108 (Samsung M2020 engine)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Stream layout, matching the output of HP's Samsung driver 3.93 for this engine:
 *   UEL + PJL header ... @PJL ENTER LANGUAGE=QPDL
 *   per page:  page header (17 bytes), band records, page footer (01 cc cc)
 *   09 + UEL
 * Band record: 0C band# width16 height16 algo(0x11) size32be,
 *   then 0x09ABCDEF (LE), compressed data, checksum32be (sum of signature + data bytes).
 * Band data for algorithm 0x11 is column-major (each byte column top to bottom)
 * and inverted (1 = white).
 * At 1200 dpi the page header's paper size is in 1/150 inch instead of 1/300.
 *
 * Manual duplex is two ordinary jobs, step 1 for the backs and step 2 for the fronts,
 * with the user moving the stack from the output tray to the input tray in between.
 * The printer has a duplex mode of its own (PJL DUPLEX = MANUAL, page source 4 for the
 * second pass, as HP's Samsung driver sends), but on the 108a under macOS it pauses
 * and then drops the second pass, with HP's driver as well. Step 1 prints the even
 * pages last-first, after a blank page if the count is odd; for long-edge binding they
 * are turned 180° within the raster area, as HP's prefilter does, by decoding and
 * re-encoding them. Step 2 prints the odd pages in order.
 */
#include "spl.h"
#include "algo0x11.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UEL "\033%-12345X"

static const spl_paper_t papers[] = {
    /* name        width  height type */
    { "Letter",      612,  792,   0 },
    { "Legal",       612, 1008,   1 },
    { "A4",          595,  842,   2 },
    { "Executive",   522,  756,   3 },
    { "Env10",       297,  684,   6 },
    { "EnvMonarch",  279,  540,   7 },
    { "EnvC5",       459,  649,   8 },
    { "EnvDL",       312,  624,   9 },
    { "B5",          516,  729,  11 },
    { "ISOB5",       499,  709,  12 },
    { "4x6",         288,  432,  13 },
    { "A5",          420,  595,  16 },
    { "FanFoldGermanLegal", 612, 936,  24 },
    { "8.5x13.5",    612,  972,  28 },
};

const spl_paper_t *spl_paper_lookup(int pts_w, int pts_h)
{
    for (size_t i = 0; i < sizeof(papers) / sizeof(papers[0]); i++)
        if (abs(papers[i].pts_w - pts_w) <= 2 && abs(papers[i].pts_h - pts_h) <= 2)
            return &papers[i];
    return NULL;
}

/*
 * The printable width (1/6 inch margins) rounded up to 32 bytes. This reproduces
 * HP's per-paper band widths at 600 and 1200 dpi, and its 600 dpi custom sizes.
 */
int spl_band_bytes(double pts_w, int dpi)
{
    double bytes = (pts_w * dpi / 72 - dpi / 3.0) / 8;
    return ((int)ceil(bytes) + 31) & ~31;
}

typedef struct {
    spl_page_t page;
    int width, rows;        /* raster size in pixels */
    long off, len;          /* page record after the header, in w->held */
} held_page_t;

struct spl_writer {
    FILE *out;
    FILE *dst;              /* where page records go: out, or held for manual duplex */
    int duplex, step, copies;
    FILE *held;
    held_page_t *pages;
    int npages, cap;
    spl_page_t page;
    int width;              /* input raster width in pixels */
    int rows;               /* rows received on this page */
    int row_bytes;          /* input raster row width */
    int band_bytes;         /* output band width (bytes) */
    int rows_in_band;
    int band_nr;
    uint8_t *band;          /* SPL_BAND_HEIGHT rows, row-major, as received */
    uint8_t *packed;        /* column-major, inverted, ready to compress */
};

spl_writer *spl_open(FILE *out)
{
    spl_writer *w = calloc(1, sizeof(*w));
    if (w)
        w->out = w->dst = out;
    return w;
}

void spl_close(spl_writer *w)
{
    if (!w)
        return;
    if (w->held)
        fclose(w->held);
    free(w->pages);
    free(w->band);
    free(w->packed);
    free(w);
}

/* PJL strings come from job titles and user names: keep them printable and quote-free. */
static void pjl_clean(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; src && *src && n + 1 < cap; src++)
        if ((unsigned char)*src >= 0x20 && *src != '"' && (unsigned char)*src < 0x7F)
            dst[n++] = *src;
    dst[n] = '\0';
}

int spl_job_begin(spl_writer *w, const spl_job_t *job)
{
    char name[80], user[80];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    pjl_clean(name, sizeof(name), job->job_name);
    pjl_clean(user, sizeof(user), job->user_name);

    fputs(UEL, w->out);     /* not in a format string: it contains '%' */
    fprintf(w->out, "@PJL DEFAULT SERVICEDATE=%04d%02d%02d\r\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    if (*user)
        fprintf(w->out, "@PJL SET USERNAME=\"%s\"\r\n", user);
    if (*name)
        fprintf(w->out, "@PJL SET JOBNAME=\"%s\"\r\n", name);
    fprintf(w->out, "@PJL SET XIGNOREFF=OFF\r\n\r\n");
    fprintf(w->out, "@PJL SET RESOLUTION = %d\r\n", job->resolution);
    if (job->resolution < 1200)     /* HP leaves it out at 1200 dpi */
        fprintf(w->out, "@PJL SET BITSPERPIXEL = 1\r\n");
    fprintf(w->out, "@PJL SET PAPERTYPE = OFF\r\n");
    fprintf(w->out, "@PJL SET DUPLEX = OFF\r\n");
    fprintf(w->out, "@PJL ENTER LANGUAGE=QPDL\r\n");

    w->duplex = job->duplex;
    w->step = job->duplex_step;
    w->copies = job->copies;
    if (w->duplex != SPL_DUPLEX_NONE) {
        /* Pages are held in an unlinked temp file until the page count is known. */
        const char *dir = getenv("TMPDIR");
        char path[1024];
        int fd;

        snprintf(path, sizeof(path), "%s/hp10x-XXXXXX", dir && *dir ? dir : "/tmp");
        if ((fd = mkstemp(path)) < 0)
            return -1;
        unlink(path);
        if (!(w->held = fdopen(fd, "w+b"))) {
            close(fd);
            return -1;
        }
        w->dst = w->held;
    }
    return ferror(w->out) ? -1 : 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_page_header(FILE *out, const spl_page_t *page)
{
    /* 1/300 inch at 600 dpi, 1/150 at 1200; HP rounds up. */
    int wu = (page->width300 * 600 + page->xres - 1) / page->xres;
    int hu = (page->height300 * 600 + page->yres - 1) / page->yres;
    uint8_t h[17];

    h[0x0] = 0x00;                          /* page header */
    h[0x1] = (uint8_t)(page->yres / 100);
    h[0x2] = (uint8_t)(page->copies >> 8);
    h[0x3] = (uint8_t)page->copies;
    h[0x4] = (uint8_t)page->paper_type;
    h[0x5] = (uint8_t)(wu >> 8);
    h[0x6] = (uint8_t)wu;
    h[0x7] = (uint8_t)(hu >> 8);
    h[0x8] = (uint8_t)hu;
    h[0x9] = (uint8_t)page->paper_source;
    h[0xa] = 0x00;
    h[0xb] = 0x00;                          /* duplex and tumble: the printer's own */
    h[0xc] = 0x00;                          /*   manual duplex, unused (see top) */
    h[0xd] = 0x00;
    h[0xe] = 0x03;                          /* QPDL version */
    h[0xf] = 0x01;
    h[0x10] = (uint8_t)(page->xres / 100);
    fwrite(h, 1, sizeof(h), out);
    return ferror(out) ? -1 : 0;
}

static int page_setup(spl_writer *w, const spl_page_t *page, int width)
{
    int row_bytes = (width + 7) / 8;

    w->page = *page;
    w->width = width;
    w->rows = 0;
    w->row_bytes = row_bytes;
    w->band_bytes = page->band_bytes ? page->band_bytes : (row_bytes + 31) & ~31;
    w->rows_in_band = 0;
    w->band_nr = 0;

    free(w->band);
    free(w->packed);
    w->band = calloc(SPL_BAND_HEIGHT, (size_t)row_bytes);
    w->packed = malloc((size_t)w->band_bytes * SPL_BAND_HEIGHT);
    return w->band && w->packed ? 0 : -1;
}

int spl_page_begin(spl_writer *w, const spl_page_t *page, int width)
{
    if (page_setup(w, page, width))
        return -1;
    if (w->duplex != SPL_DUPLEX_NONE) {
        /* The header depends on where the page lands, so it's written at the end. */
        if (w->npages == w->cap) {
            int cap = w->cap ? w->cap * 2 : 16;
            held_page_t *p = realloc(w->pages, (size_t)cap * sizeof(*p));
            if (!p)
                return -1;
            w->pages = p;
            w->cap = cap;
        }
        w->pages[w->npages] = (held_page_t){ *page, width, 0, ftell(w->held), 0 };
        return 0;
    }
    return write_page_header(w->out, page);
}

static int flush_band(spl_writer *w)
{
    const size_t bb = (size_t)w->band_bytes, h = SPL_BAND_HEIGHT;
    const size_t copy = (size_t)w->row_bytes < bb ? (size_t)w->row_bytes : bb;
    int blank = 1;
    uint8_t hdr[11], sig[4] = { 0xEF, 0xCD, 0xAB, 0x09 }, ck[4];
    uint8_t *data;
    size_t size;
    uint32_t sum = 0;

    /* Rows not yet filled (end of page) stay white. */
    if (w->rows_in_band < SPL_BAND_HEIGHT)
        memset(w->band + (size_t)w->rows_in_band * w->row_bytes, 0,
               (SPL_BAND_HEIGHT - w->rows_in_band) * (size_t)w->row_bytes);

    for (size_t y = 0; y < h && blank; y++)
        for (size_t x = 0; x < copy; x++)
            if (w->band[y * w->row_bytes + x]) {
                blank = 0;
                break;
            }

    if (!blank) {
        /* Transpose to column-major and invert; columns past the raster are white. */
        for (size_t x = 0; x < bb; x++)
            for (size_t y = 0; y < h; y++)
                w->packed[x * h + y] = x < copy ? (uint8_t)~w->band[y * w->row_bytes + x] : 0xFF;

        data = a11_compress(w->packed, bb * h, &size);
        if (!data)
            return -1;
        for (size_t i = 0; i < 4; i++)
            sum += sig[i];
        for (size_t i = 0; i < size; i++)
            sum += data[i];

        hdr[0] = 0x0C;
        hdr[1] = (uint8_t)w->band_nr;
        hdr[2] = (uint8_t)((bb * 8) >> 8);
        hdr[3] = (uint8_t)(bb * 8);
        hdr[4] = (uint8_t)(h >> 8);
        hdr[5] = (uint8_t)h;
        hdr[6] = 0x11;
        put_be32(hdr + 7, (uint32_t)(size + 8));
        put_be32(ck, sum);

        fwrite(hdr, 1, sizeof(hdr), w->dst);
        fwrite(sig, 1, sizeof(sig), w->dst);
        fwrite(data, 1, size, w->dst);
        fwrite(ck, 1, sizeof(ck), w->dst);
        free(data);
    }

    w->band_nr++;
    w->rows_in_band = 0;
    return ferror(w->dst) ? -1 : 0;
}

int spl_page_row(spl_writer *w, const uint8_t *row)
{
    memcpy(w->band + (size_t)w->rows_in_band * w->row_bytes, row, (size_t)w->row_bytes);
    w->rows++;
    if (++w->rows_in_band == SPL_BAND_HEIGHT)
        return flush_band(w);
    return 0;
}

static int page_footer(spl_writer *w)
{
    uint8_t f[3] = { 0x01, (uint8_t)(w->page.copies >> 8), (uint8_t)w->page.copies };

    if (w->rows_in_band && flush_band(w))
        return -1;
    fwrite(f, 1, sizeof(f), w->dst);
    return ferror(w->dst) ? -1 : 0;
}

int spl_page_end(spl_writer *w)
{
    if (page_footer(w))
        return -1;
    if (w->duplex != SPL_DUPLEX_NONE) {
        held_page_t *hp = &w->pages[w->npages++];
        hp->rows = w->rows;
        hp->len = ftell(w->held) - hp->off;
    }
    return 0;
}

/* Decode a held page's bands into rows of 1-bit pixels (1 = black). */
static uint8_t *decode_held(spl_writer *w, const held_page_t *hp)
{
    const int rb = (hp->width + 7) / 8;
    uint8_t *bmp = calloc((size_t)hp->rows ? (size_t)hp->rows : 1, (size_t)rb), h[11];
    uint8_t *data = NULL, *raw = NULL;
    long left = hp->len;

    if (!bmp || fseek(w->held, hp->off, SEEK_SET))
        goto fail;
    while (left > 3 && fread(h, 1, 1, w->held) == 1 && h[0] == 0x0C) {
        if (fread(h + 1, 1, 10, w->held) != 10)
            goto fail;
        size_t bb = (((size_t)h[2] << 8) | h[3]) / 8, bh = ((size_t)h[4] << 8) | h[5];
        size_t size = ((size_t)h[7] << 24) | ((size_t)h[8] << 16) | ((size_t)h[9] << 8) | h[10];
        free(data);
        free(raw);
        data = malloc(size);
        raw = malloc(bb * bh);
        if (!data || !raw || size < 8 || fread(data, 1, size, w->held) != size ||
            a11_decompress(data + 4, size - 8, raw, bb * bh) != (long)(bb * bh))
            goto fail;
        /* Column-major and inverted -> row-major, 1 = black. */
        for (size_t x = 0; x < bb && x < (size_t)rb; x++)
            for (size_t y = 0; y < bh; y++) {
                size_t row = (size_t)h[1] * bh + y;
                if (row < (size_t)hp->rows)
                    bmp[row * rb + x] = (uint8_t)~raw[x * bh + y];
            }
        left -= 11 + (long)size;
    }
    free(data);
    free(raw);
    return bmp;
fail:
    free(data);
    free(raw);
    free(bmp);
    return NULL;
}

/* Re-encode a held page turned 180° within its raster area: bottom row first, mirrored. */
static int send_turned(spl_writer *w, const held_page_t *hp)
{
    const int rb = (hp->width + 7) / 8;
    uint8_t *bmp = decode_held(w, hp), *row = malloc((size_t)rb);
    int status = 0;

    if (!bmp || !row || page_setup(w, &hp->page, hp->width)) {
        status = -1;
        goto done;
    }
    w->dst = w->out;
    for (int y = hp->rows; y-- > 0 && !status; ) {
        const uint8_t *src = bmp + (size_t)y * rb;

        memset(row, 0, (size_t)rb);
        for (int x = 0; x < hp->width; x++)
            if (src[x >> 3] & (0x80 >> (x & 7))) {
                int d = hp->width - 1 - x;
                row[d >> 3] |= (uint8_t)(0x80 >> (d & 7));
            }
        status = spl_page_row(w, row);
    }
    if (!status)
        status = page_footer(w);
    w->dst = w->held;
done:
    free(bmp);
    free(row);
    return status;
}

/* Send held page `i`, or a blank page for -1; `turn` turns it 180°. */
static int emit_held(spl_writer *w, int i, int turn)
{
    const held_page_t *hp = &w->pages[i < 0 ? w->npages - 1 : i];
    uint8_t buf[65536], blank[3] = { 0x01, 0x00, 0x01 };

    if (write_page_header(w->out, &hp->page))
        return -1;
    if (i < 0) {
        fwrite(blank, 1, sizeof(blank), w->out);
        return ferror(w->out) ? -1 : 0;
    }
    if (turn)
        return send_turned(w, hp);
    if (fseek(w->held, hp->off, SEEK_SET))
        return -1;
    for (long left = hp->len; left > 0; ) {
        size_t n = fread(buf, 1, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf), w->held);
        if (!n)
            return -1;
        fwrite(buf, 1, n, w->out);
        left -= (long)n;
    }
    return ferror(w->out) ? -1 : 0;
}

/* Do held pages a and b have the same content? Copies of a page encode identically. */
static int same_page(spl_writer *w, int a, int b)
{
    const held_page_t *pa = &w->pages[a], *pb = &w->pages[b];
    uint8_t x[65536], y[65536];

    if (pa->len != pb->len)
        return 0;
    for (long off = 0; off < pa->len; off += (long)sizeof(x)) {
        size_t n = pa->len - off < (long)sizeof(x) ? (size_t)(pa->len - off) : sizeof(x);
        if (fseek(w->held, pa->off + off, SEEK_SET) || fread(x, 1, n, w->held) != n ||
            fseek(w->held, pb->off + off, SEEK_SET) || fread(y, 1, n, w->held) != n ||
            memcmp(x, y, n))
            return 0;
    }
    return 1;
}

/*
 * The copies arrive as one run of pages, collated (1 2 3 1 2 3) or not (1 1 2 2 3 3),
 * depending on options that differ between macOS and CUPS, so tell from the pages.
 * Returns the number of copies found, and sets *uncollated.
 */
static int find_copies(spl_writer *w, int *uncollated)
{
    int n = w->npages, copies = w->copies, len, collated_ok = 1, uncollated_ok = 1;

    *uncollated = 0;
    if (copies < 2 || n % copies)
        return 1;
    len = n / copies;
    for (int i = 0; i + len < n && collated_ok; i++)
        collated_ok = same_page(w, i, i + len);
    for (int i = 0; i < n && uncollated_ok && !collated_ok; i++)
        uncollated_ok = i % copies == 0 || same_page(w, i, i - 1);
    if (collated_ok)
        return copies;
    *uncollated = uncollated_ok;
    return uncollated_ok ? copies : 1;
}

static int send_duplex(spl_writer *w)
{
    int uncollated, copies = find_copies(w, &uncollated);
    int n = w->npages, len = n / copies, m = 0, *seq, status = 0;

    if (len == 1) {
        /* Single pages have no backs: step 1 prints nothing, step 2 everything. */
        for (int i = 0; i < n && w->step == 2; i++)
            if (emit_held(w, i, 0))
                return -1;
        return 0;
    }
    /*
     * Each copy gets its own sheets: pages in collated order, and a blank back after a
     * copy with an odd page count. Backs are the even positions, sent last-first; fronts
     * the odd positions, in order.
     */
    if (!(seq = malloc((size_t)(n + copies) * sizeof(*seq))))
        return -1;
    for (int c = 0; c < copies; c++) {
        for (int j = 0; j < len; j++)
            seq[m++] = uncollated ? j * copies + c : c * len + j;
        if (len % 2)
            seq[m++] = -1;
    }
    if (w->step == 1)
        for (int k = m - 1; k > 0 && !status; k -= 2)
            status = emit_held(w, seq[k], w->duplex == SPL_DUPLEX_LONG);
    else
        for (int k = 0; k < m && !status; k += 2)
            status = emit_held(w, seq[k], 0);
    free(seq);
    return status;
}

int spl_job_end(spl_writer *w)
{
    if (w->duplex != SPL_DUPLEX_NONE && w->npages && send_duplex(w))
        return -1;
    fputc(0x09, w->out);
    fputs(UEL, w->out);
    fflush(w->out);
    return ferror(w->out) ? -1 : 0;
}
