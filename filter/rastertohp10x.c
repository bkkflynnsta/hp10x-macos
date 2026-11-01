/*
 * rastertohp10x - CUPS filter: CUPS raster -> SPL/QPDL for HP Laser 103/107/108
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Usage (CUPS): rastertohp10x job user title copies options [file]
 * Accepts 8-bit gray (K, W or sGray; screened here) or 1-bit (K or W) raster.
 * Options read from argv[5]: TonerSave=True|False
 * Copies are made upstream (the PPD sets cupsManualCopies), so each page is sent once.
 */
#include "../src/halftone.h"
#include "../src/spl.h"

#include <cups/cups.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static volatile sig_atomic_t canceled;

static void on_term(int sig)
{
    (void)sig;
    canceled = 1;
}

/* White-is-one color spaces need inverting to get darkness / black bits. */
static int is_white_space(cups_cspace_t cs)
{
    return cs == CUPS_CSPACE_W || cs == CUPS_CSPACE_SW;
}

int main(int argc, char **argv)
{
    int fd = 0, page = 0, nopts, status = 0, toner_save;
    cups_option_t *opts = NULL;
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    spl_writer *w;
    spl_job_t job = { 0 };
    const char *v;
    uint8_t *line = NULL, *ring = NULL, *saved = NULL;

    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
        return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: Unable to open raster file");
        return 1;
    }
    signal(SIGTERM, on_term);

    nopts = cupsParseOptions(argv[5], 0, &opts);
    v = cupsGetOption("TonerSave", nopts, opts);
    /* The PPD option is Boolean (True/False); accept On/Yes too for manual use. */
    toner_save = v && (!strcasecmp(v, "True") || !strcasecmp(v, "On") || !strcasecmp(v, "Yes"));
    job.job_name = argv[3];
    job.user_name = argv[2];

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    w = spl_open(stdout);

    while (!canceled && cupsRasterReadHeader2(ras, &hdr)) {
        int width = (int)hdr.cupsWidth, out_bytes = (width + 7) / 8;
        int invert = is_white_space(hdr.cupsColorSpace);
        int gray = hdr.cupsBitsPerPixel == 8;
        const spl_paper_t *paper = spl_paper_lookup((int)hdr.PageSize[0], (int)hdr.PageSize[1]);
        spl_page_t sp = { 0 };

        if (!(gray || hdr.cupsBitsPerPixel == 1) || hdr.cupsBitsPerColor != hdr.cupsBitsPerPixel) {
            fprintf(stderr, "ERROR: Unsupported raster: %u bits per pixel, color space %d\n",
                    hdr.cupsBitsPerPixel, hdr.cupsColorSpace);
            status = 1;
            break;
        }
        if (page == 0) {
            job.resolution = (int)hdr.HWResolution[0];
            if (spl_job_begin(w, &job)) {
                status = 1;
                break;
            }
        }
        page++;
        fprintf(stderr, "INFO: Printing page %d\n", page);
        fprintf(stderr, "PAGE: %d 1\n", page);

        sp.xres = (int)hdr.HWResolution[0];
        sp.yres = (int)hdr.HWResolution[1];
        sp.copies = 1;
        sp.paper_source = hdr.ManualFeed || hdr.MediaPosition == 2 ? 2 : 1;
        sp.width300 = (int)((hdr.PageSize[0] * 300 + 36) / 72);     /* round to nearest, as HP does */
        sp.height300 = (int)((hdr.PageSize[1] * 300 + 36) / 72);
        sp.paper_type = paper ? paper->paper_type : 0;
        if (paper)
            sp.band_bytes = sp.xres >= 1200 ? paper->band_bytes_1200 : paper->band_bytes_600;

        free(line);
        free(ring);
        free(saved);
        line = malloc(hdr.cupsBytesPerLine);
        /* Three 1-bit rows: toner save needs the rows above and below. */
        ring = malloc(3 * (size_t)out_bytes);
        saved = malloc((size_t)out_bytes);
        if (!line || !ring || !saved || spl_page_begin(w, &sp, out_bytes)) {
            status = 1;
            break;
        }

        unsigned y = 0;
        for (; y < hdr.cupsHeight && !canceled; y++) {
            uint8_t *row = ring + (y % 3) * (size_t)out_bytes;

            if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) < 1)
                break;
            if (gray) {
                if (invert)
                    for (int x = 0; x < width; x++)
                        line[x] = (uint8_t)~line[x];
                halftone_row(line, width, (int)y, row);
            } else {
                for (int x = 0; x < out_bytes; x++)
                    row[x] = invert ? (uint8_t)~line[x] : line[x];
                if (width % 8)          /* clear padding bits past the last pixel */
                    row[out_bytes - 1] &= (uint8_t)(0xFF << (8 - width % 8));
            }
            if (!toner_save) {
                if (spl_page_row(w, row)) {
                    status = 1;
                    break;
                }
            } else if (y > 0) {
                /* Emit the previous row now that the one below it is known. */
                tonersave_row(y > 1 ? ring + ((y - 2) % 3) * (size_t)out_bytes : NULL,
                              ring + ((y - 1) % 3) * (size_t)out_bytes, row,
                              out_bytes, (int)y - 1, saved);
                if (spl_page_row(w, saved)) {
                    status = 1;
                    break;
                }
            }
        }
        if (!status && toner_save && y > 0) {
            tonersave_row(y > 1 ? ring + ((y - 2) % 3) * (size_t)out_bytes : NULL,
                          ring + ((y - 1) % 3) * (size_t)out_bytes, NULL,
                          out_bytes, (int)y - 1, saved);
            status = spl_page_row(w, saved) ? 1 : 0;
        }
        if (status || spl_page_end(w)) {
            status = 1;
            break;
        }
    }

    if (page && !status && spl_job_end(w))
        status = 1;
    if (!page && !status)
        fprintf(stderr, "ERROR: No pages found\n");

    spl_close(w);
    cupsRasterClose(ras);
    cupsFreeOptions(nopts, opts);
    free(line);
    free(ring);
    free(saved);
    if (fd)
        close(fd);
    return canceled ? 0 : status;
}
