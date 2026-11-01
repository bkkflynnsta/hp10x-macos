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
 * screen phase; `dpi` (600 or 1200) picks the screen and tone curve. `out` must hold
 * (width + 7) / 8 bytes.
 */
void halftone_row(const uint8_t *dark, int width, int y, int dpi, uint8_t *out);

/*
 * Toner save, as HP's driver does it: a lighter tone curve that also screens solid
 * black, while edges (a neighbour less than half as dark) print as normal, so text
 * keeps a solid outline. `rows` holds the darkness rows y-r .. y+r, where r is 1 at
 * 600 dpi and 2 at 1200 dpi; rows off the page are NULL. Writes (width + 7) / 8 bytes.
 */
void tonersave_row(const uint8_t *const *rows, int width, int y, int dpi, uint8_t *out);

#endif
