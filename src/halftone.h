/*
 * halftone.h - 8-bit gray to 1-bit conversion with a 45° clustered-dot screen
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HP10X_HALFTONE_H
#define HP10X_HALFTONE_H

#include <stdint.h>

/*
 * Convert one row of `width` darkness values (0 = white, 255 = black) to packed
 * 1-bit pixels (1 = black, MSB first). `y` is the page row, used to pick the
 * screen phase. `out` must hold (width + 7) / 8 bytes.
 */
void halftone_row(const uint8_t *dark, int width, int y, uint8_t *out);

/*
 * Toner save, as HP's driver does it: keep edges solid and clear a diagonal grid of
 * pixels inside solid areas (about a quarter of the interior). `prev`/`next` are the
 * neighbouring 1-bit rows (NULL at the page edges); writes `nbytes` bytes to `out`.
 */
void tonersave_row(const uint8_t *prev, const uint8_t *cur, const uint8_t *next,
                   int nbytes, int y, uint8_t *out);

#endif
