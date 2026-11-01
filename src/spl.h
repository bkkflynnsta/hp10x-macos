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

typedef struct {
    const char *job_name;   /* may be NULL */
    const char *user_name;  /* may be NULL */
    int resolution;         /* 600 or 1200 */
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
    int band_bytes_600;     /* band width in bytes at 600 dpi, 0 = derive */
    int band_bytes_1200;
} spl_paper_t;

/* Find a paper by size in points (±2pt). Returns NULL for custom sizes. */
const spl_paper_t *spl_paper_lookup(int pts_w, int pts_h);

typedef struct spl_writer spl_writer;

spl_writer *spl_open(FILE *out);
int spl_job_begin(spl_writer *w, const spl_job_t *job);

/*
 * Start a page whose raster rows are `row_bytes` wide (1 bit per pixel, 1 = black,
 * MSB = leftmost pixel). Rows are fed with spl_page_row(); blank bands are skipped.
 */
int spl_page_begin(spl_writer *w, const spl_page_t *page, int row_bytes);
int spl_page_row(spl_writer *w, const uint8_t *row);
int spl_page_end(spl_writer *w);

int spl_job_end(spl_writer *w);
void spl_close(spl_writer *w);

#endif
