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
 */
#include "spl.h"
#include "algo0x11.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UEL "\033%-12345X"

static const spl_paper_t papers[] = {
    /* name        width  height type  600   1200 */
    { "Letter",      612,  792,   0,  640, 1248 },
    { "Legal",       612, 1008,   1,  640, 1248 },
    { "A4",          595,  842,   2,  608, 1216 },
    { "Executive",   522,  756,   3,  544, 1056 },
    { "Env10",       297,  684,   6,  288,  576 },
    { "EnvMonarch",  279,  540,   7,  288,  544 },
    { "EnvC5",       459,  649,   8,  480,  928 },
    { "EnvDL",       312,  624,   9,  320,  608 },
    { "B5",          516,  729,  11,  544, 1056 },
    { "ISOB5",       499,  709,  12,  512,  992 },
    { "4x6",         288,  432,  13,  288,  576 },
    { "A5",          420,  595,  16,  416,  832 },
    { "FanFoldGermanLegal", 612, 936,  24,  640, 1248 },
    { "8.5x13.5",    612,  972,  28,  640, 1248 },
};

const spl_paper_t *spl_paper_lookup(int pts_w, int pts_h)
{
    for (size_t i = 0; i < sizeof(papers) / sizeof(papers[0]); i++)
        if (abs(papers[i].pts_w - pts_w) <= 2 && abs(papers[i].pts_h - pts_h) <= 2)
            return &papers[i];
    return NULL;
}

struct spl_writer {
    FILE *out;
    spl_page_t page;
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
        w->out = out;
    return w;
}

void spl_close(spl_writer *w)
{
    if (!w)
        return;
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
    fprintf(w->out, "@PJL SET BITSPERPIXEL = 1\r\n");
    fprintf(w->out, "@PJL SET PAPERTYPE = OFF\r\n");
    fprintf(w->out, "@PJL SET DUPLEX = OFF\r\n");
    fprintf(w->out, "@PJL ENTER LANGUAGE=QPDL\r\n");
    return ferror(w->out) ? -1 : 0;
}

int spl_page_begin(spl_writer *w, const spl_page_t *page, int row_bytes)
{
    uint8_t h[17];

    w->page = *page;
    w->row_bytes = row_bytes;
    w->band_bytes = page->band_bytes ? page->band_bytes : (row_bytes + 31) & ~31;
    w->rows_in_band = 0;
    w->band_nr = 0;

    free(w->band);
    free(w->packed);
    w->band = calloc(SPL_BAND_HEIGHT, (size_t)row_bytes);
    w->packed = malloc((size_t)w->band_bytes * SPL_BAND_HEIGHT);
    if (!w->band || !w->packed)
        return -1;

    h[0x0] = 0x00;                          /* page header */
    h[0x1] = (uint8_t)(page->yres / 100);
    h[0x2] = (uint8_t)(page->copies >> 8);
    h[0x3] = (uint8_t)page->copies;
    h[0x4] = (uint8_t)page->paper_type;
    h[0x5] = (uint8_t)(page->width300 >> 8);
    h[0x6] = (uint8_t)page->width300;
    h[0x7] = (uint8_t)(page->height300 >> 8);
    h[0x8] = (uint8_t)page->height300;
    h[0x9] = (uint8_t)page->paper_source;
    h[0xa] = 0x00;
    h[0xb] = 0x00;                          /* duplex: off */
    h[0xc] = 0x00;                          /* tumble */
    h[0xd] = 0x00;
    h[0xe] = 0x03;                          /* QPDL version */
    h[0xf] = 0x01;
    h[0x10] = (uint8_t)(page->xres / 100);
    fwrite(h, 1, sizeof(h), w->out);
    return ferror(w->out) ? -1 : 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
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

        fwrite(hdr, 1, sizeof(hdr), w->out);
        fwrite(sig, 1, sizeof(sig), w->out);
        fwrite(data, 1, size, w->out);
        fwrite(ck, 1, sizeof(ck), w->out);
        free(data);
    }

    w->band_nr++;
    w->rows_in_band = 0;
    return ferror(w->out) ? -1 : 0;
}

int spl_page_row(spl_writer *w, const uint8_t *row)
{
    memcpy(w->band + (size_t)w->rows_in_band * w->row_bytes, row, (size_t)w->row_bytes);
    if (++w->rows_in_band == SPL_BAND_HEIGHT)
        return flush_band(w);
    return 0;
}

int spl_page_end(spl_writer *w)
{
    uint8_t f[3] = { 0x01, (uint8_t)(w->page.copies >> 8), (uint8_t)w->page.copies };

    if (w->rows_in_band && flush_band(w))
        return -1;
    fwrite(f, 1, sizeof(f), w->out);
    return ferror(w->out) ? -1 : 0;
}

int spl_job_end(spl_writer *w)
{
    fputc(0x09, w->out);
    fputs(UEL, w->out);
    fflush(w->out);
    return ferror(w->out) ? -1 : 0;
}
