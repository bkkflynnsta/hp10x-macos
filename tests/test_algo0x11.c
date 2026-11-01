/*
 * test_algo0x11 - round-trip tests for the 0x11 compressor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../src/algo0x11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAND (640 * 128)    /* Letter band at 600 dpi */

static int failures;

static void roundtrip(const char *name, const uint8_t *in, size_t size)
{
    size_t csize;
    uint8_t *c = a11_compress(in, size, &csize);
    uint8_t *out = malloc(size);
    long got = c ? a11_decompress(c, csize, out, size) : -2;

    if (got != (long)size || memcmp(in, out, size)) {
        printf("FAIL %-22s size=%zu got=%ld\n", name, size, got);
        failures++;
    } else {
        printf("ok   %-22s %7zu -> %7zu bytes\n", name, size, csize);
    }
    free(c);
    free(out);
}

int main(void)
{
    uint8_t *buf = malloc(BAND);
    uint32_t seed = 12345;

    memset(buf, 0xFF, BAND);
    roundtrip("all white", buf, BAND);

    memset(buf, 0x00, BAND);
    roundtrip("all black", buf, BAND);

    for (size_t i = 0; i < BAND; i++) {
        seed = seed * 1103515245 + 12345;
        buf[i] = (uint8_t)(seed >> 16);
    }
    roundtrip("random (incompressible)", buf, BAND);

    for (size_t i = 0; i < BAND; i++)
        buf[i] = (i / 128) % 7 == 0 ? 0x0F : 0xFF;      /* vertical stripes, column-major */
    roundtrip("stripes", buf, BAND);

    for (size_t i = 0; i < BAND; i++)
        buf[i] = (uint8_t)((i % 128) < 64 ? 0xAA : 0x55);
    roundtrip("checker", buf, BAND);

    for (size_t n = 1; n <= 300; n += 37) {               /* sizes around the literal prefix */
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)(i * 7);
        roundtrip("short", buf, n);
    }

    /* Corrupt streams must be rejected, not overrun the output. */
    {
        uint8_t junk[200], out[64];
        memset(junk, 0, sizeof(junk));
        junk[0] = 0xFF;                                  /* absurd literal prefix */
        if (a11_decompress(junk, sizeof(junk), out, sizeof(out)) != -1) {
            printf("FAIL corrupt prefix accepted\n");
            failures++;
        } else {
            printf("ok   corrupt stream rejected\n");
        }
    }

    free(buf);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
