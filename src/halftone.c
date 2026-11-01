/*
 * halftone.c - 8-bit gray to 1-bit conversion with a 45° clustered-dot screen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Laser engines hold clustered dots far more consistently than the isolated pixels
 * of error diffusion, so gray is rendered with a round-dot screen whose cells lie on
 * the lattice (4,4)/(4,-4): about 106 lpi at 600 dpi, 45°, 33 levels. At 1200 dpi the
 * cells are twice as many pixels across, the same 106 lpi with 129 levels, as HP's
 * High Resolution mode does. Pure black and pure white are exact, which keeps text
 * and line art sharp.
 *
 * Toner save, as HP's driver does it, prints through a lighter tone curve that also
 * screens solid black (to about 72%), except on edges, so text keeps a solid outline.
 */
#include "halftone.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#define MAX_TILE 16

typedef struct { double in, out; } point_t;

/*
 * Tone curves (percent in -> percent coverage out), measured from HP's driver 3.93 on
 * gray ramps: they deepen midtones and hold back the darkest grays. HP's 1200 dpi
 * curve runs a few points lighter than its 600 dpi one.
 */
static const point_t curve600[] = {
    {   0.0,   0.0 }, {  12.5,  11.6 }, {  25.0,  25.7 }, {  35.9,  44.2 },
    {  46.9,  53.4 }, {  57.8,  61.8 }, {  67.2,  69.5 }, {  76.6,  76.1 },
    {  84.4,  82.6 }, {  92.2,  87.3 }, { 100.0, 100.0 },
};
static const point_t curve1200[] = {
    {   0.0,   0.0 }, {   7.8,   5.2 }, {  15.3,  11.7 }, {  23.1,  22.4 },
    {  30.2,  34.2 }, {  37.3,  42.2 }, {  44.3,  47.8 }, {  51.0,  51.9 },
    {  57.6,  57.7 }, {  63.9,  63.0 }, {  69.8,  68.0 }, {  75.7,  71.6 },
    {  80.8,  73.9 }, {  85.9,  78.8 }, {  90.6,  83.3 }, {  94.5,  85.9 },
    {  96.5,  86.8 }, { 100.0, 100.0 },
};

/* Toner save curves, measured the same way with HP's Toner Save on. */
static const point_t save600[] = {
    {   0.0,   0.0 }, {   7.8,   4.6 }, {  15.3,   9.3 }, {  23.1,  14.7 },
    {  30.2,  20.8 }, {  37.3,  28.5 }, {  44.3,  37.9 }, {  51.0,  43.7 },
    {  57.6,  48.1 }, {  63.9,  51.5 }, {  69.8,  54.8 }, {  75.7,  57.4 },
    {  80.8,  60.8 }, {  85.9,  63.6 }, {  90.6,  66.5 }, {  94.5,  68.8 },
    { 100.0,  72.1 },
};
static const point_t save1200[] = {
    {   0.0,   0.0 }, {   7.8,   4.0 }, {  15.3,   7.0 }, {  23.1,  12.2 },
    {  30.2,  19.5 }, {  37.3,  26.3 }, {  44.3,  34.9 }, {  51.0,  41.3 },
    {  57.6,  45.5 }, {  63.9,  48.0 }, {  69.8,  51.0 }, {  75.7,  53.2 },
    {  80.8,  56.4 }, {  85.9,  59.8 }, {  90.6,  62.6 }, {  94.5,  65.0 },
    { 100.0,  68.2 },
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int tile;                           /* screen repeats every `tile` pixels */
    const point_t *curve, *save;
    size_t ncurve, nsave;
    pthread_once_t once;
    uint8_t thresh[MAX_TILE][MAX_TILE];
    uint8_t transfer[256];
    uint8_t save_transfer[256];
} screen_t;

static screen_t screen600 = { 8, curve600, save600, N(curve600), N(save600), PTHREAD_ONCE_INIT,
                              {{0}}, {0}, {0} };
static screen_t screen1200 = { 16, curve1200, save1200, N(curve1200), N(save1200), PTHREAD_ONCE_INIT,
                               {{0}}, {0}, {0} };

typedef struct { double spot; int x, y, half; } cell_t;

static int cmp_spot(const void *a, const void *b)
{
    const cell_t *p = a, *q = b;
    if (p->spot != q->spot)
        return p->spot > q->spot ? -1 : 1;
    /* Equal spots occur once in each of the tile's two dots: alternate between them. */
    int pa = (p->x / p->half + p->y / p->half) & 1, qa = (q->x / q->half + q->y / q->half) & 1;
    if (pa != qa)
        return pa - qa;
    return p->y != q->y ? p->y - q->y : p->x - q->x;
}

static void make_transfer(const point_t *curve, size_t n, uint8_t *transfer)
{
    for (int v = 0; v < 256; v++) {
        double in = v * 100.0 / 255, out = in;
        for (size_t i = 1; i < n; i++)
            if (in <= curve[i].in) {
                double f = (in - curve[i - 1].in) / (curve[i].in - curve[i - 1].in);
                out = curve[i - 1].out + f * (curve[i].out - curve[i - 1].out);
                break;
            }
        transfer[v] = (uint8_t)lround(out * 255 / 100);
    }
    transfer[0] = 0;
}

static void build_screen(screen_t *sc)
{
    const int tile = sc->tile;
    cell_t c[MAX_TILE * MAX_TILE];
    int n = 0;

    for (int y = 0; y < tile; y++)
        for (int x = 0; x < tile; x++) {
            double px = x + 0.5, py = y + 0.5;
            double s = (px + py) / tile, t = (px - py) / tile;
            double fs = s - floor(s) - 0.5, ft = t - floor(t) - 0.5;
            /* Round dot: pixels nearest the cell centre darken first. */
            c[n++] = (cell_t){ -(fs * fs + ft * ft), x, y, tile / 2 };
        }
    qsort(c, n, sizeof(c[0]), cmp_spot);
    for (int k = 0; k < n; k++)
        /* Thresholds spread over 1..254 so 0 stays white and 255 stays black. */
        sc->thresh[c[k].y][c[k].x] = (uint8_t)(1 + (k * 253 + (n - 1) / 2) / (n - 1));

    make_transfer(sc->curve, sc->ncurve, sc->transfer);
    make_transfer(sc->save, sc->nsave, sc->save_transfer);
    /* Keep the ends exact so text and solids are untouched (toner save screens black). */
    sc->transfer[255] = 255;
}

static void build600(void)
{
    build_screen(&screen600);
}

static void build1200(void)
{
    build_screen(&screen1200);
}

static screen_t *screen_for(int dpi)
{
    screen_t *sc = dpi >= 1200 ? &screen1200 : &screen600;

    pthread_once(&sc->once, sc == &screen1200 ? build1200 : build600);
    return sc;
}

static void screen_row(const screen_t *sc, const uint8_t *transfer, const uint8_t *dark,
                       int width, int y, uint8_t *out)
{
    const uint8_t *tr = sc->thresh[y % sc->tile];
    int nbytes = (width + 7) / 8;

    for (int b = 0; b < nbytes; b++) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            int x = b * 8 + i;
            if (x < width && transfer[dark[x]] >= tr[x % sc->tile])
                v |= (uint8_t)(0x80 >> i);
        }
        out[b] = v;
    }
}

void halftone_row(const uint8_t *dark, int width, int y, int dpi, uint8_t *out)
{
    const screen_t *sc = screen_for(dpi);

    screen_row(sc, sc->transfer, dark, width, y, out);
}

void tonersave_row(const uint8_t *const *rows, int width, int y, int dpi, uint8_t *out)
{
    const screen_t *sc = screen_for(dpi);
    const int r = dpi >= 1200 ? 2 : 1;
    const uint8_t *cur = rows[r];
    uint8_t tmp[16384], *edge = width <= (int)sizeof(tmp) ? tmp : malloc((size_t)width);

    if (!edge) {
        screen_row(sc, sc->save_transfer, cur, width, y, out);
        return;
    }
    /*
     * An edge pixel has a neighbour within r pixels across or down that is less than half
     * as dark; past the page counts as white. Edges print as normal, the rest is saved.
     */
    for (int x = 0; x < width; x++) {
        int half = cur[x] / 2, e = 0;
        for (int k = 1; k <= r && !e && half; k++)
            e = x - k < 0 || cur[x - k] < half || x + k >= width || cur[x + k] < half ||
                !rows[r - k] || rows[r - k][x] < half || !rows[r + k] || rows[r + k][x] < half;
        edge[x] = (uint8_t)e;
    }
    screen_row(sc, sc->save_transfer, cur, width, y, out);
    for (int x = 0; x < width; x++)
        if (edge[x] && sc->transfer[cur[x]] >= sc->thresh[y % sc->tile][x % sc->tile])
            out[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    if (edge != tmp)
        free(edge);
}
