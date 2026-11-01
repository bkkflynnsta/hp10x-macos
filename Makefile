# HP Laser 103/107/108 driver for macOS
# SPDX-License-Identifier: GPL-2.0-or-later

ARCHS    ?= -arch arm64 -arch x86_64
MINOS    ?= -mmacosx-version-min=12.0
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-deprecated-declarations $(ARCHS) $(MINOS)
BUILD    := build

LIB_SRC  := src/spl.c src/algo0x11.c src/halftone.c
LIB_HDR  := src/spl.h src/algo0x11.h src/halftone.h

TOOLS    := $(BUILD)/spldecode
FILTER   := $(BUILD)/rastertohp10x

all: $(FILTER) $(TOOLS)

$(BUILD):
	mkdir -p $@

$(BUILD)/spldecode: tools/spldecode.c src/algo0x11.c $(LIB_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/spldecode.c src/algo0x11.c

$(BUILD)/rastertohp10x: filter/rastertohp10x.c $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ filter/rastertohp10x.c $(LIB_SRC) -lcups -lcupsimage

$(BUILD)/test_algo0x11: tests/test_algo0x11.c src/algo0x11.c src/algo0x11.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_algo0x11.c src/algo0x11.c

test: all $(BUILD)/test_algo0x11
	$(BUILD)/test_algo0x11
	tests/compare-with-hp.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test clean

GUARD    := $(BUILD)/hp10x-ippusb-guard

$(BUILD)/hp10x-ippusb-guard: installer/ippusb-guard.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ installer/ippusb-guard.c -framework IOKit -framework CoreFoundation

all: $(GUARD)
