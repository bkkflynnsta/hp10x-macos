/*
 * spl.h - SPL/QPDL v3 writer for the HP Laser 103/107/108 (Samsung M2020 engine)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HP10X_SPL_H
#define HP10X_SPL_H

#include <stdint.h>
#include <stdio.h>

#define SPL_BAND_HEIGHT 128
#define SPL_PAPER_CUSTOM 21     /* QPDL paper code for sizes not in the table */

enum { SPL_DUPLEX_NONE, SPL_DUPLEX_LONG, SPL_DUPLEX_SHORT };

typedef struct {
    const char *job_name;   /* may be NULL */
    const char *user_name;  /* may be NULL */
    int resolution;         /* 600 or 1200 */
    int duplex;             /* SPL_DUPLEX_*: manual duplex binding */
    int duplex_step;        /* manual duplex: 1 prints the backs, 2 the fronts */
    int copies;             /* manual duplex: the pages hold this many copies of the document */
} spl_job_t;

typedef struct {
    int xres, yres;         /* dpi */
    int paper_type;         /* QPDL paper code, see spl_paper_lookup() */
    int width300;           /* paper size in 1/300 inch */
    int height300;
    int copies;
    int paper_source;       /* 1 = auto tray, 2 = manual */
    int band_bytes;         /* bytes per band row; 0 = derive from raster width */
} spl_page_t;

typedef struct {
    const char *name;       /* PPD PageSize name */
    int pts_w, pts_h;       /* size in points */
    int paper_type;         /* QPDL paper code */
} spl_paper_t;

/* Find a paper by size in points (±2pt). Returns NULL for custom sizes. */
const spl_paper_t *spl_paper_lookup(int pts_w, int pts_h);

/* Band width in bytes for paper `pts_w` points wide at `dpi`, as HP's driver sets it. */
int spl_band_bytes(double pts_w, int dpi);

typedef struct spl_writer spl_writer;

spl_writer *spl_open(FILE *out);
int spl_job_begin(spl_writer *w, const spl_job_t *job);

/*
 * Start a page whose raster is `width` pixels wide; rows are (width + 7) / 8 bytes,
 * 1 bit per pixel, 1 = black, MSB = leftmost pixel. Rows are fed with spl_page_row();
 * blank bands are skipped. In a manual-duplex job pages are held until spl_job_end(),
 * which sends step 1, the backs (even pages, last-first), or step 2, the fronts (odd
 * pages). Each copy gets its own sheets.
 */
int spl_page_begin(spl_writer *w, const spl_page_t *page, int width);
int spl_page_row(spl_writer *w, const uint8_t *row);
int spl_page_end(spl_writer *w);

int spl_job_end(spl_writer *w);
void spl_close(spl_writer *w);

#endif
