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
src/spl.[ch]        QPDL v3 writer: PJL header, page/band records, paper table
src/halftone.[ch]   45° clustered-dot screen, HP tone curve, toner save
filter/             CUPS filter
tools/spldecode     decode any QPDL stream into PBM pages and check every checksum
tools/pbm2png.py    crop/scale a PBM into a PNG for viewing
tests/              unit tests and a pixel comparison against HP's own driver
installer/          guard daemon, launchd plist, pkg scripts, build-pkg.sh
```

## Build and test

```sh
make            # universal (arm64 + x86_64) binaries in build/
make test       # compressor round-trips + comparison against HP's driver
installer/build-pkg.sh   # build/hp10x-<version>.pkg
```

The comparison test needs HP's Samsung driver 3.93 and a queue that uses its "Samsung M2020 Series" PPD. Without them it's skipped. The test renders the same raster with both filters, decodes both outputs, and checks:

- the gray-ramp tone is within 3 points per patch (currently 1.2);
- the text pixels differ by no more than 0.5% (currently 0.08%).

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
page:   00 yres/100 copies16 papertype width16 height16 source 00 00 00 00 03 01 xres/100
band:   0C band# width16 height16 11 size32 | EF CD AB 09 | data | checksum32
footer: 01 copies16
end:    09 ESC%-12345X
```

- Sizes are big-endian. Paper width and height are in 1/300 inch, rounded to the nearest unit.
- Bands are 128 rows tall. The band width (bytes) comes from a per-paper table, not from the raster width: Letter is 640 at 600 dpi, A4 is 608.
- Band data is stored **column-major** (each byte column from top to bottom) and **inverted** (1 = white).
- Blank bands are left out.
- The checksum is the sum of the signature bytes plus the data bytes.
- All 14 paper sizes produce exactly the page header and band width HP's driver does.

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

- "High Resolution" mode. HP's version is 600 dpi with a private flag.
- Manual duplex.
- Custom page sizes.
- Testing on models other than the 108a. The guard matches any "HP Laser 103/107/108*".

## Licensing

- Files from SpliX (`src/algo0x11.*`) are GPL-2.0-only.
- Everything else is GPL-2.0-or-later.
- The combined driver is distributed under GPL-2.0-only.
- Not affiliated with HP Inc.
