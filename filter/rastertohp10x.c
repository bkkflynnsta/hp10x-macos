/*
 * rastertohp10x - CUPS filter: CUPS raster -> SPL/QPDL for HP Laser 103/107/108
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Usage (CUPS): rastertohp10x job user title copies options [file]
 * Accepts 8-bit gray (K, W or sGray; screened here) or 1-bit (K or W) raster, at
 * 600 or 1200 dpi. Both become darkness rows (0 = white, 255 = black) before screening.
 * Options read from argv[5]: TonerSave=True|False,
 *   ManualDuplex=None|LongEdgeBacks|LongEdgeFronts|ShortEdgeBacks|ShortEdgeFronts
 * Copies are made upstream (the PPD sets cupsManualCopies), so each page is sent once.
 */
#include "../src/halftone.h"
#include "../src/spl.h"

#include <cups/cups.h>
#include <cups/raster.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
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

/* Screen darkness row `y` (toner save also reads the rows around it) and send it on. */
static int emit_row(spl_writer *w, const uint8_t *dark, int ring, int reach, unsigned y,
                    unsigned have, int width, int dpi, int toner_save, uint8_t *out)
{
    if (!toner_save) {
        halftone_row(dark + (size_t)(y % ring) * width, width, (int)y, dpi, out);
    } else {
        const uint8_t *around[5];
        for (int k = -reach; k <= reach; k++) {
            long r = (long)y + k;
            around[k + reach] = r >= 0 && r < (long)have ? dark + (size_t)(r % ring) * width : NULL;
        }
        tonersave_row(around, width, (int)y, dpi, out);
    }
    return spl_page_row(w, out);
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
    uint8_t *line = NULL, *dark = NULL, *out = NULL;

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
    /* ManualDuplex=<LongEdge|ShortEdge><Backs|Fronts>: step 1 prints the backs, step 2 the fronts. */
    v = cupsGetOption("ManualDuplex", nopts, opts);
    if (v && (!strncasecmp(v, "LongEdge", 8) || !strncasecmp(v, "ShortEdge", 9))) {
        const char *step = v + (tolower((unsigned char)v[0]) == 'l' ? 8 : 9);
        job.duplex = tolower((unsigned char)v[0]) == 'l' ? SPL_DUPLEX_LONG : SPL_DUPLEX_SHORT;
        job.duplex_step = !strcasecmp(step, "Backs") ? 1 : !strcasecmp(step, "Fronts") ? 2 : 0;
        if (!job.duplex_step)
            job.duplex = SPL_DUPLEX_NONE;
    }
    /* The copies are made upstream; manual duplex gives each its own sheets. */
    job.copies = atoi(argv[4]);
    job.job_name = argv[3];
    job.user_name = argv[2];

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    w = spl_open(stdout);

    while (!canceled && cupsRasterReadHeader2(ras, &hdr)) {
        int width = (int)hdr.cupsWidth, out_bytes = (width + 7) / 8;
        int invert = is_white_space(hdr.cupsColorSpace);
        int gray = hdr.cupsBitsPerPixel == 8;
        /* cupsPageSize keeps the fractions of a custom size; older headers lack it. */
        double pw = hdr.cupsPageSize[0] > 0 ? hdr.cupsPageSize[0] : hdr.PageSize[0];
        double ph = hdr.cupsPageSize[1] > 0 ? hdr.cupsPageSize[1] : hdr.PageSize[1];
        const spl_paper_t *paper = spl_paper_lookup((int)lround(pw), (int)lround(ph));
        spl_page_t sp = { 0 };

        if (hdr.HWResolution[0] != hdr.HWResolution[1] ||
            (hdr.HWResolution[0] != 600 && hdr.HWResolution[0] != 1200)) {
            fprintf(stderr, "ERROR: Unsupported resolution %ux%u dpi\n",
                    hdr.HWResolution[0], hdr.HWResolution[1]);
            status = 1;
            break;
        }
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
        sp.width300 = (int)lround(pw * 300 / 72);     /* round to nearest, as HP does */
        sp.height300 = (int)lround(ph * 300 / 72);
        sp.paper_type = paper ? paper->paper_type : SPL_PAPER_CUSTOM;
        sp.band_bytes = spl_band_bytes(pw, sp.xres);

        free(line);
        free(dark);
        free(out);
        line = malloc(hdr.cupsBytesPerLine);
        /* A ring of darkness rows: toner save looks `reach` rows above and below. */
        int reach = toner_save ? (sp.xres >= 1200 ? 2 : 1) : 0, ring = 2 * reach + 1;
        dark = malloc((size_t)ring * width);
        out = malloc((size_t)out_bytes);
        if (!line || !dark || !out || spl_page_begin(w, &sp, width)) {
            status = 1;
            break;
        }

        unsigned y = 0;
        for (; y < hdr.cupsHeight && !canceled; y++) {
            uint8_t *d = dark + (size_t)(y % ring) * width;

            if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) < 1)
                break;
            if (gray)
                for (int x = 0; x < width; x++)
                    d[x] = invert ? (uint8_t)~line[x] : line[x];
            else
                for (int x = 0; x < width; x++)
                    d[x] = ((line[x >> 3] >> (7 - (x & 7))) & 1) != invert ? 255 : 0;
            /* Row y - reach now has all its neighbours. */
            if ((int)y >= reach && emit_row(w, dark, ring, reach, y - reach, y + 1, width, sp.xres,
                                            toner_save, out)) {
                status = 1;
                break;
            }
        }
        for (unsigned e = y > (unsigned)reach ? y - reach : 0; !status && e < y; e++)
            if (emit_row(w, dark, ring, reach, e, y, width, sp.xres, toner_save, out))
                status = 1;
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
    free(dark);
    free(out);
    if (fd)
        close(fd);
    return canceled ? 0 : status;
}
