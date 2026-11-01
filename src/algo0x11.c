/*
 * algo0x11.c - QPDL compression algorithm 0x11
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * Ported to C from SpliX (algo0x11.cpp), Copyright (C) 2007-2008 Aurélien Croc (AP²C)
 *
 * Token stream after the literal prefix:
 *   0nnnnnnn                 literal run of n+1 bytes follows
 *   1ccccccc hhpppppp        copy (c | hh<<7) + 3 bytes from `table[p]` bytes back
 */
#include "algo0x11.h"

#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t count, offset; } occur_t;

static int cmp_occurs_desc(const void *a, const void *b)
{
    const occur_t *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return x->offset < y->offset ? -1 : (x->offset > y->offset);
}

/* Pick the 64 back-reference distances that repeat most often in a sample of the data. */
static void build_table(const uint8_t *data, size_t size, uint16_t table[A11_TABLE_SIZE])
{
    occur_t occ[A11_SAMPLE_RATE];
    int has_one = 0;

    for (size_t i = 0; i < A11_SAMPLE_RATE; i++) {
        occ[i].count = 0;
        occ[i].offset = (uint32_t)i;
    }
    for (size_t i = A11_SAMPLE_RATE; i < size; i += A11_SAMPLE_RATE) {
        uint8_t b = data[i];
        for (size_t j = 1; j < A11_SAMPLE_RATE; j++)
            if (data[i - j] == b)
                occ[j - 1].count++;
    }
    qsort(occ, A11_SAMPLE_RATE, sizeof(occ[0]), cmp_occurs_desc);

    for (size_t i = 0; i < A11_TABLE_SIZE; i++) {
        table[i] = (uint16_t)(occ[i].offset + 1);
        if (table[i] == 1)
            has_one = 1;
    }
    /* Distance 1 compresses runs of identical bytes, so always keep it. */
    if (!has_one)
        table[A11_TABLE_SIZE - 1] = 1;
}

uint8_t *a11_compress(const uint8_t *in, size_t size, size_t *out_size)
{
    uint16_t table[A11_TABLE_SIZE];
    size_t prefix = 0, r, w = 4, raw_count = 0, raw_pos = 0;
    /* Worst case: every 128 literals cost one extra byte. */
    size_t cap = 4 + 2 * A11_TABLE_SIZE + size + size / A11_MAX_RAW + 16;
    uint8_t *out = malloc(cap);

    if (!out)
        return NULL;

    build_table(in, size, table);
    for (size_t i = 0; i < A11_TABLE_SIZE; i++, w += 2) {
        out[w] = (uint8_t)table[i];
        out[w + 1] = (uint8_t)(table[i] >> 8);
        if (table[i] > prefix)
            prefix = table[i];
    }

    /* The first `prefix` bytes are sent as-is so every back-reference has history. */
    if (prefix > A11_MAX_RAW)
        prefix = A11_MAX_RAW;
    if (prefix > size)
        prefix = size;
    out[0] = (uint8_t)prefix;
    out[1] = (uint8_t)(prefix >> 8);
    out[2] = (uint8_t)(prefix >> 16);
    out[3] = (uint8_t)(prefix >> 24);
    memcpy(out + w, in, prefix);
    w += prefix;
    r = prefix;

    while (r < size) {
        size_t max_copy = size - r > A11_MAX_COPY ? A11_MAX_COPY : size - r;

        if (max_copy >= 2) {
            size_t best_len = 0, best_idx = 0;

            for (size_t i = 0; i < A11_TABLE_SIZE; i++) {
                size_t len;
                const uint8_t *src;

                if (table[i] > r)
                    continue;
                src = in + r - table[i];
                for (len = 0; len < max_copy; len++)
                    if (in[r + len] != src[len])
                        break;
                if (len > best_len) {
                    best_len = len;
                    best_idx = i;
                }
            }

            if (best_len > A11_MIN_COPY) {
                size_t c = best_len - 3;

                r += best_len;
                out[w] = (uint8_t)(A11_COPY_FLAG | (c & 0x7F));
                out[w + 1] = (uint8_t)(((c >> 1) & 0xC0) | (best_idx & 0x3F));
                w += 2;
                if (raw_count) {
                    out[raw_pos] = (uint8_t)(raw_count - 1);
                    raw_count = 0;
                }
                continue;
            }
        }

        /* Literal byte: open a new run, or close a full one. */
        raw_count++;
        if (raw_count == 1) {
            raw_pos = w++;
        } else if (raw_count == A11_MAX_RAW) {
            out[raw_pos] = 0x7F;
            raw_count = 0;
        }
        out[w++] = in[r++];
    }
    if (raw_count)
        out[raw_pos] = (uint8_t)(raw_count - 1);

    *out_size = w;
    return out;
}

long a11_decompress(const uint8_t *in, size_t size, uint8_t *out, size_t out_cap)
{
    uint16_t table[A11_TABLE_SIZE];
    size_t r, w = 0, prefix;

    if (size < 4 + 2 * A11_TABLE_SIZE)
        return -1;
    prefix = in[0] | (in[1] << 8) | ((size_t)in[2] << 16) | ((size_t)in[3] << 24);
    r = 4;
    for (size_t i = 0; i < A11_TABLE_SIZE; i++, r += 2)
        table[i] = (uint16_t)(in[r] | (in[r + 1] << 8));

    if (prefix > out_cap || r + prefix > size)
        return -1;
    memcpy(out, in + r, prefix);
    r += prefix;
    w = prefix;

    while (r < size) {
        uint8_t t = in[r++];

        if (t & A11_COPY_FLAG) {
            size_t len, dist;

            if (r >= size)
                return -1;
            len = ((t & 0x7F) | ((size_t)(in[r] & 0xC0) << 1)) + 3;
            dist = table[in[r] & 0x3F];
            r++;
            if (dist == 0 || dist > w || w + len > out_cap)
                return -1;
            /* Byte-by-byte: source and destination may overlap. */
            for (size_t i = 0; i < len; i++, w++)
                out[w] = out[w - dist];
        } else {
            size_t len = (size_t)t + 1;

            if (r + len > size || w + len > out_cap)
                return -1;
            memcpy(out + w, in + r, len);
            r += len;
            w += len;
        }
    }
    return (long)w;
}
