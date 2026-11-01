# Developing hp10x

hp10x is an open-source macOS driver for the HP Laser 103/107/108, which are host-based printers on a Samsung M2020-series engine. It has three parts:

| Part | What it does |
|---|---|
| `filter/rastertohp10x` | CUPS filter: 8-bit or 1-bit CUPS raster in, SPL/QPDL out |
| `ppd/HP-Laser-10x.ppd` | Tells macOS the page sizes and options, and that the filter wants 600 dpi 8-bit gray |
| `installer/hp10x-ippusb-guard` | launchd daemon that stops macOS's `ippusbd` from claiming the printer |

## Layout

```
src/algo0x11.[ch]   QPDL compression 0x11 (ported from SpliX, GPL-2.0-only)
src/spl.[ch]        QPDL v3 writer: PJL header, page/band records, paper table, manual duplex
src/halftone.[ch]   45° clustered-dot screens and HP tone curves (600/1200 dpi), toner save
filter/             CUPS filter
tools/spldecode     decode any QPDL stream into PBM pages and check every checksum
tools/pbm2png.py    crop/scale a PBM into a PNG for viewing
tests/              unit tests and a pixel comparison against HP's own driver
installer/          guard daemon, launchd plist, pkg scripts, build-pkg.sh
scripts/            maintenance scripts run from the repo; not installed by the package
```

## Build and test

```sh
make            # universal (arm64 + x86_64) binaries in build/
make test       # unit tests, manual-duplex filter test, comparison against HP's driver
installer/build-pkg.sh   # build/hp10x-<version>.pkg
```

The comparison test needs HP's Samsung driver 3.93 and a queue that uses its "Samsung M2020 Series" PPD. Without them it's skipped. It renders the test page with both filters, decodes both outputs, and checks:

| Mode | Page header | Gray-ramp tone (≤ 3 points) | Text |
|---|---|---|---|
| 600 dpi | identical | 1.2 points | 0.08% of pixels differ (≤ 0.5%) |
| 1200 dpi | identical except the size, which HP's Mac driver halves | 0.8 points | ink weight −2.3% (≤ 5%) |
| Toner save, 600 dpi | | 1.0 points | ink weight −0.2% |
| Toner save, 1200 dpi | | 0.5 points | ink weight −1.3% |
| Custom 250×400 pt | identical | | |

Text is compared by weight at 1200 dpi because HP's High Resolution doubles its 600 dpi rendering, while ours is rendered at 1200 dpi, so the edges differ by design. In toner save, HP's screen has rounder holes than ours.

`tests/test_spl.c` checks the band widths against HP's table and the manual-duplex page order; `tests/duplex-filter.sh` runs the filter's two manual-duplex steps and checks which pages are turned.

To try a change without installing it, point a copy of the PPD at the build:

```sh
sed "s#/Library/Printers/hp10x/Filters/rastertohp10x#$PWD/build/rastertohp10x#" \
    ppd/HP-Laser-10x.ppd > /tmp/dev.ppd
cupsfilter -p /tmp/dev.ppd -m printer/foo -o PageSize=A4 -e file.pdf > out.prn
build/spldecode out.prn out          # writes out-1.pbm, out-2.pbm, ...
lp -d <queue> -o raw out.prn         # send to a real printer
```

## The printer language (QPDL v3, as this engine uses it)

This was verified byte for byte against HP's driver 3.93.

```
ESC%-12345X @PJL ... @PJL ENTER LANGUAGE=QPDL\r\n
page:   00 yres/100 copies16 papertype width16 height16 source 00 duplex tumble 00 03 01 xres/100
band:   0C band# width16 height16 11 size32 | EF CD AB 09 | data | checksum32
footer: 01 copies16
end:    09 ESC%-12345X
```

- Sizes are big-endian. Paper width and height are in 1/300 inch, rounded to the nearest unit, at both resolutions. HP's Mac driver halves them at 1200 dpi (1/150 inch), and the 108a starts those pages and then drops them. HP's Linux driver (ULD 1.00.39) keeps 1/300 inch, and those pages print.
- Paper sizes not in the table use paper type 21 (custom). HP's driver accepts 3×5 in to US Legal.
- Bands are 128 rows tall. The band width in bytes is the printable width rounded up to 32: `ceil((width_px − dpi/3) / 8)`, rounded up to a multiple of 32. That reproduces HP's per-paper table at both resolutions (Letter is 640 at 600 dpi, A4 is 608) and its custom sizes at 600 dpi. HP's own 1200 dpi custom bands are narrower than the page, which looks like a bug.
- Band data is stored **column-major** (each byte column from top to bottom) and **inverted** (1 = white).
- Blank bands are left out.
- The checksum is the sum of the signature bytes plus the data bytes.
- All 14 paper sizes produce exactly the page header and band width HP's driver does.
- At 1200 dpi the PJL says `RESOLUTION = 1200` and leaves out `BITSPERPIXEL`. HP's Mac High Resolution renders at 600 dpi and doubles it; the filter takes a 1200 dpi raster.
- 1200 dpi was verified on paper on the 108a (firmware R000105): sharper text, crisp vertical lines and an even gray ramp. HP's Linux driver screens its darker 1200 dpi grays almost solid black, so the tone curve stays the one from HP's Mac driver.

### Manual duplex

The printer has a manual duplex mode of its own, which HP's Samsung driver uses (its `prefilter` orders and turns the pages, `rastertosec` sets the headers):

- PJL `SET DUPLEX = MANUAL` and `SET BINDING = LONGEDGE` or `SHORTEDGE`; every page header has duplex = 1, and tumble 1 on the backs and 0 on the fronts for long edge (the reverse for short edge).
- The fronts have page source 4. The printer blinks its `!` light before the first of them.

**On the 108a under macOS this doesn't work.** The printer pauses after the backs and then drops the fronts, with HP's driver too, whether or not its button is pressed. hp10x therefore prints manual duplex as two ordinary jobs, with HP's page order and turning:

- **Step 1, backs:** the even pages last-first, after a blank page if the count is odd. For long edge they're turned 180° within the raster area.
- The user moves the stack from the output tray to the input tray as it lies. The tray feeds from the top and prints the side facing up, and output lands face down, so this puts the right sheet first, blank side up, with the right edge leading. This was verified on paper with a 5-page A4 document.
- **Step 2, fronts:** the odd pages in order.
- A one-page document has no backs: step 1 prints nothing.

The writer holds the encoded pages in a temp file and sends the step's pages at the end of the job; it decodes, turns and re-encodes the long-edge backs. Copies are made upstream (`cupsManualCopies`), so with several copies the writer works out from the page content whether they arrived collated or not, and gives each copy its own sheets.

HP's `rastertosec` crashes in manual duplex mode unless `/Library/Caches/com.sec.printer` holds "jobid flip", which its `prefilter` writes. To run it by hand, write that file first.

### Toner save

HP prints through a lighter tone curve that also screens solid black (to 72% at 600 dpi, 68% at 1200), but pixels on an edge, with a 4-neighbour less than half as dark, print as normal. So text keeps a solid outline (two pixels wide at 1200 dpi) around a screened inside, and saves about 22% of its toner.

Things that *don't* work, so don't spend time on them:

- The printer advertises `URF` and `PWGRaster`, but raw Apple raster or PWG raster (with or without a PJL `ENTER LANGUAGE`) doesn't print.
- Its IPP-over-USB interface returns empty responses.

## The `ippusbd` problem

The USB descriptor gives both interfaces an alternate setting 1 with IPP-over-USB (class 7, protocol 4). When the printer connects, macOS's `smd` creates a launchd job, `com.apple.print.ippusb.<vendor>.<product>.<serial>`, which starts `ippusbd` on demand. `ippusbd` then claims both interfaces, and the `usb://` queue sits at "offline".

The guard runs on every HP USB attach (an IOKit matching event, with `idVendor` inside `IOPropertyMatch`; a top-level `idVendor` doesn't match `IOUSBHostDevice`). It disables and removes that job, checking a second time 5 seconds later. `uninstall.sh` re-enables the jobs.

Run `build/hp10x-ippusb-guard --dry-run` to see which labels it would disable.

## Signing and releases

`build-pkg.sh` signs the binaries with `SIGN_ID`, which defaults to the pointcleardigital.com Apple Development certificate. That's fine for your own Macs. A public release needs:

```sh
SIGN_ID="Developer ID Application: …" INSTALLER_ID="Developer ID Installer: …" installer/build-pkg.sh
xcrun notarytool submit build/hp10x-<ver>.pkg --keychain-profile <profile> --wait
xcrun stapler staple build/hp10x-<ver>.pkg
```

## Not supported yet

- Testing on models other than the 108a. The guard matches any "HP Laser 103/107/108*".
- HP's Edge Enhancement, Skip Blank Pages, Brightness/Contrast and Paper Type options.

## Licensing

- Files from SpliX (`src/algo0x11.*`) are GPL-2.0-only.
- Everything else is GPL-2.0-or-later.
- The combined driver is distributed under GPL-2.0-only.
- Not affiliated with HP Inc.
