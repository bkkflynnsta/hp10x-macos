/*
 * halftone.c - 8-bit gray to 1-bit conversion with a 45° clustered-dot screen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Laser engines hold clustered dots far more consistently than the isolated pixels
 * of error diffusion, so gray is rendered with a round-dot screen whose cells lie on
 * the lattice (4,4)/(4,-4): about 106 lpi at 600 dpi, 45°, 33 levels. Pure black and
 * pure white are exact, which keeps text and line art sharp.
 */
#include "halftone.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#define TILE 8

static uint8_t thresh[TILE][TILE];
static uint8_t transfer[256];

/*
 * Tone curve (percent in -> percent coverage out), measured from HP's driver 3.93 on
 * a gray ramp: it deepens midtones and holds back the darkest grays.
 */
static const double curve[][2] = {
    {   0.0,   0.0 }, {  12.5,  11.6 }, {  25.0,  25.7 }, {  35.9,  44.2 },
    {  46.9,  53.4 }, {  57.8,  61.8 }, {  67.2,  69.5 }, {  76.6,  76.1 },
    {  84.4,  82.6 }, {  92.2,  87.3 }, { 100.0, 100.0 },
};
static pthread_once_t once = PTHREAD_ONCE_INIT;

typedef struct { double spot; int x, y; } cell_t;

static int cmp_spot(const void *a, const void *b)
{
    const cell_t *p = a, *q = b;
    if (p->spot != q->spot)
        return p->spot > q->spot ? -1 : 1;
    /* Equal spots occur once in each of the tile's two dots: alternate between them. */
    int pa = (p->x / 4 + p->y / 4) & 1, qa = (q->x / 4 + q->y / 4) & 1;
    if (pa != qa)
        return pa - qa;
    return p->y != q->y ? p->y - q->y : p->x - q->x;
}

static void build_screen(void)
{
    cell_t c[TILE * TILE];
    int n = 0;

    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            double px = x + 0.5, py = y + 0.5;
            double s = (px + py) / TILE, t = (px - py) / TILE;
            double fs = s - floor(s) - 0.5, ft = t - floor(t) - 0.5;
            /* Round dot: pixels nearest the cell centre darken first. */
            c[n++] = (cell_t){ -(fs * fs + ft * ft), x, y };
        }
    qsort(c, n, sizeof(c[0]), cmp_spot);
    for (int k = 0; k < n; k++)
        /* Thresholds spread over 1..254 so 0 stays white and 255 stays black. */
        thresh[c[k].y][c[k].x] = (uint8_t)(1 + (k * 253 + (n - 1) / 2) / (n - 1));

    for (int v = 0; v < 256; v++) {
        double in = v * 100.0 / 255, out = in;
        for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++)
            if (in <= curve[i][0]) {
                double f = (in - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]);
                out = curve[i - 1][1] + f * (curve[i][1] - curve[i - 1][1]);
                break;
            }
        transfer[v] = (uint8_t)lround(out * 255 / 100);
    }
    /* Keep the ends exact so text and solids are untouched. */
    transfer[0] = 0;
    transfer[255] = 255;
}

void halftone_row(const uint8_t *dark, int width, int y, uint8_t *out)
{
    const uint8_t *tr;
    int nbytes = (width + 7) / 8;

    pthread_once(&once, build_screen);
    tr = thresh[y % TILE];
    for (int b = 0; b < nbytes; b++) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            int x = b * 8 + i;
            if (x < width && transfer[dark[x]] >= tr[x % TILE])
                v |= (uint8_t)(0x80 >> i);
        }
        out[b] = v;
    }
}

static int bit(const uint8_t *row, int x, int nbytes)
{
    return row && x >= 0 && x < nbytes * 8 && (row[x >> 3] & (0x80 >> (x & 7)));
}

void tonersave_row(const uint8_t *prev, const uint8_t *cur, const uint8_t *next,
                   int nbytes, int y, uint8_t *out)
{
    for (int b = 0; b < nbytes; b++) {
        uint8_t v = cur[b];
        if (!v) {
            out[b] = 0;
            continue;
        }
        for (int i = 0; i < 8; i++) {
            int x = b * 8 + i;
            /* Hole lattice: every 4th pixel, shifted by 2 on alternate rows. */
            if ((x + (y & 1) * 2) % 4)
                continue;
            if (!(v & (0x80 >> i)))
                continue;
            if (bit(prev, x, nbytes) && bit(next, x, nbytes) &&
                bit(cur, x - 1, nbytes) && bit(cur, x + 1, nbytes))
                v &= (uint8_t)~(0x80 >> i);
        }
        out[b] = v;
    }
}
