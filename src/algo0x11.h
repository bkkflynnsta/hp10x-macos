/*
 * algo0x11.h - QPDL compression algorithm 0x11 (LZ-style, 64-entry offset table)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * Ported to C from SpliX (algo0x11.cpp), Copyright (C) 2007-2008 Aurélien Croc (AP²C)
 */
#ifndef HP10X_ALGO0X11_H
#define HP10X_ALGO0X11_H

#include <stddef.h>
#include <stdint.h>

#define A11_SAMPLE_RATE     0x800   /* bytes between occurrence samples */
#define A11_TABLE_SIZE      0x40    /* entries in the offset table */
#define A11_MAX_RAW         0x80    /* longest literal run */
#define A11_MAX_COPY        0x202   /* longest back-reference */
#define A11_MIN_COPY        0x2     /* back-references must be longer than this */
#define A11_COPY_FLAG       0x80

/*
 * Compress `size` bytes of `in`. On success returns a malloc'd buffer laid out as
 *   uint32le literal-prefix length, uint16le table[64], prefix bytes, token stream
 * and stores its length in *out_size. Returns NULL on allocation failure.
 */
uint8_t *a11_compress(const uint8_t *in, size_t size, size_t *out_size);

/*
 * Decompress into `out` (capacity `out_cap`). Returns bytes written, or -1 if the
 * stream is malformed or would overflow `out`.
 */
long a11_decompress(const uint8_t *in, size_t size, uint8_t *out, size_t out_cap);

#endif
