# HP Laser 108a on macOS

How to set up the HP Laser 103 / 107 / 108 family (USB, "a" models) on a modern Mac, including Apple Silicon and macOS 27.

Tested on: Mac mini M2, macOS 27.0.1, CUPS 2.3.4, Samsung Printer Driver 3.93.

## Why this printer needs a driver

The 108a is a **host-based** printer. It has no PostScript or PCL interpreter, so the Mac has to turn each page into the printer's own raster language, Samsung **SPL** (also called QPDL), before sending it. The printer is a rebadged Samsung Xpress M2020-series engine, and it identifies itself over USB like this:

```
MFG:HP;MDL:HP Laser 103 107 108;CMD:SPL,URF,FWV,PIC,EXT,PWGRaster;MODE:SPL3
```

macOS ships no SPL driver, and AirPrint doesn't work over this printer's USB port. The way that works is Samsung's own macOS driver with the **Samsung M2020 Series** model selected.

Despite the `URF` and `PWGRaster` entries above, these were tested on firmware `R000105` and none of them work:

- Its IPP-over-USB interface returns empty responses.
- Raw Apple raster (URF) and PWG raster don't print, either bare or after a PJL `ENTER LANGUAGE` command.
- Raw raster can leave the printer stuck in `STATUS:BUSY` until it's switched off and on. Only send it SPL from the Samsung driver.

## Two ways to set it up

- **hp10x, the open-source driver from this project** (recommended):
  1. Download `hp10x-<version>.pkg` from [Releases](https://github.com/bkkflynnsta/hp10x-macos/releases/latest) and open it. It's signed and notarized by Apple, and needs macOS 12 or later on Apple Silicon or Intel.
  2. Connect the printer, then in **System Settings → Printers & Scanners** add it and choose **HP Laser 103/107/108 (hp10x)** under **Use**.

  It also fixes the "printer is offline" problem described below automatically, with no Terminal commands. It's been tested on a 108a with macOS 27.0.1. To remove it, run `sudo /Library/Printers/hp10x/uninstall.sh`. To build it yourself, see [docs/DEVELOPING.md](docs/DEVELOPING.md).
- **HP's Samsung driver**, set up as described in the rest of this guide.

### hp10x print options

In the print dialog, these are under **Printer Options** (or **Printer Features**):

| Option | Choices |
|---|---|
| Page Size | Letter, Legal, A4, A5, B5, Executive, Folio, Oficio, 4×6 postcard, four envelope sizes, or a custom size from 3×5 in (76×127 mm) up to US Legal |
| Resolution | 600 dpi, or 1200 dpi (High Resolution) for sharper text and finer grays |
| Toner Save | Prints text as a solid outline with a lighter inside, and grays lighter; saves about a fifth of the toner |
| Two-Sided Printing (Manual) | Off, or Step 1 / Step 2 for long-edge or short-edge binding (below) |

Set the tray's paper guides snugly against the paper. A loose guide lets the sheet feed at an angle, which cuts off one edge.

#### Two-sided printing

The printer can't turn paper over, so you print the document twice:

1. Print with **Two-Sided Printing (Manual): Long Edge, Step 1: Backs**, or Short Edge for flip-up pages like a calendar. It prints the even pages, last first, and a blank sheet first if the page count is odd. With Long Edge they come out upside down; that's intended.
2. Take the printed stack from the output tray and put it straight into the input tray, **exactly as it lies**: still face down, not turned over and not turned around. You don't need to remove the blank paper underneath.
3. Print the same document again with **Step 2: Fronts**. It prints the odd pages onto the backs of those sheets.

Use the same copies setting for both steps. With several copies, each copy gets its own sheets.

The printer has a built-in two-sided mode that pauses between sides, which HP's Samsung driver uses, but on the 108a under macOS it pauses and then drops the second side, with HP's driver as well. That's why hp10x uses two jobs.

| Item | Notes |
|---|---|
| USB cable | USB-A/B, or a USB-C to USB-B cable for newer Macs |
| **Samsung Printer Driver for macOS, v3.93 or later** | Download it from HP's support site under the Samsung Xpress SL-M2020 / M2020 series. HP distributes Samsung's drivers. |
| Admin password | Needed to install the driver |

v3.93 is a universal binary (Apple Silicon + Intel) signed with HP's Developer ID, so it **does not need Rosetta**. That matters on macOS 27 and later.

To check the driver after installing it:

```sh
file /Library/Printers/Samsung/UPD/Filters/rastertosec
# should list: x86_64 and arm64
```

## Setup

1. Install the Samsung driver package and restart if it asks you to.
2. Connect the printer **directly** to the Mac, or to a powered hub, and turn it on.
3. Open **System Settings → Printers & Scanners → Add Printer, Scanner or Fax…**
4. Choose **HP Laser 103 107 108** (kind: USB).
5. In **Use**, pick **Select Software…** and search for **`M2020`**, then choose **Samsung M2020 Series**.
   Don't pick "Generic PostScript" or "Generic PCL". The printer can't read either, so jobs will vanish without printing.
6. Click **Add**, then print a test page.

### Terminal alternative

```sh
lpadmin -p HP_Laser_108a -E \
  -D "HP Laser 108a" \
  -v "$(lpinfo -v | awk '/HP%20Laser%20103/ {print $2; exit}')" \
  -m "Library/Printers/PPDs/Contents/Resources/Samsung M2020 Series.gz" \
  -o printer-is-shared=false
lpadmin -d HP_Laser_108a          # optional: make it the default printer
lp -d HP_Laser_108a /etc/hosts     # test print
```

## Troubleshooting

Run `./hp108-check.sh` from this folder. It checks every item below and prints the results.

### Queue stuck on "The printer is offline" (macOS 27)

This is the main failure on macOS 27. The printer's USB descriptor gives both of its interfaces an alternate setting for IPP-over-USB (class 7, protocol 4):

```
interface 0 alt 0: class 7   protocol 2   <- normal USB printing (Samsung driver)
interface 0 alt 1: class 7   protocol 4   <- IPP-over-USB
interface 1 alt 0: class 255 protocol 1   <- vendor channel
interface 1 alt 1: class 7   protocol 4   <- IPP-over-USB
```

When the printer connects, macOS creates a launchd job for `ippusbd` that listens on a localhost port. As soon as anything checks the printer (Print Center, a status poll), `ippusbd` starts. It takes **exclusive** control of both interfaces and switches them to IPP-over-USB. The printer's IPP-over-USB doesn't actually work: it returns empty responses. So the Samsung driver's `usb://` connection is locked out, CUPS shows `connecting-to-device` / "The printer is offline", and jobs sit in the queue. Because it depends on when something happens to check the printer, the failure seems to come and go.

To confirm it:

```sh
ioreg -r -c IOUSBHostInterface -l -w0 | grep -A4 '"HP Laser 108a"' | grep UsbExclusiveOwner
# "UsbExclusiveOwner" = "pid 12345, ippusbd"   <- this is the problem
```

To fix it for now, stop `ippusbd` in Terminal.app so the normal USB backend can connect:

```sh
sudo killall ippusbd
```

Jobs sent while `ippusbd` held the printer stay stuck at "The printer is offline" even after that, because CUPS doesn't retry them. Cancel them and print again:

```sh
cancel -a HP_Laser_108a
cupsenable HP_Laser_108a
```

In **Print Center**, deleting the stuck job and printing again does the same thing.

This only lasts until the printer is next switched on or reconnected.

#### Permanent fix: disable `ippusbd` for this printer only

When the printer connects, macOS creates a per-printer launchd job named `com.apple.print.ippusb.<manufacturer>.<model>.<serial>`. That job starts `ippusbd` whenever anything checks the printer. Disabling it for this printer's serial number stops the takeover and leaves other IPP-over-USB printers alone:

```sh
# Build the label from what the printer reports over USB
SP=$(system_profiler SPUSBHostDataType 2>/dev/null | grep -A10 'HP Laser 10[378]')
MFG=$(echo "$SP" | sed -n 's/.*Manufacturer: //p' | head -1)        # e.g. HP Inc.
MDL=$(echo "$SP" | grep -o 'HP Laser 10[378][a-z]*' | head -1)        # e.g. HP Laser 108a
SER=$(echo "$SP" | sed -n 's/.*Serial Number: //p' | head -1)
L="com.apple.print.ippusb.$MFG.$MDL.$SER"; echo "$L"

sudo launchctl disable "system/$L"
sudo launchctl bootout "system/$L" 2>/dev/null   # remove it now if it's loaded
sudo killall ippusbd 2>/dev/null
```

This survives restarts and power-cycles. With it in place, macOS no longer creates the job when the printer connects, so the interfaces stay in normal mode. This was verified on macOS 27.0.1 over three power-cycles. To undo it, run `sudo launchctl enable "system/$L"`.

If you have *other* printers on USB that use IPP-over-USB (`ippusb://` in `lpstat -v`), they keep working. Their jobs have different labels.

### Jobs report "completed" but nothing prints

This is the most common failure, including after a macOS upgrade. CUPS marks the job done once the USB write succeeds, even if the printer drops the data. Work through these in order:

1. **Check the printer's own status.**
   ```sh
   /usr/libexec/cups/backend/usb | grep -i "laser 103"
   ```
   Look for `STATUS:` at the end of the device ID. This printer sometimes reports `STATUS:BUSY` even when it's working. If it says `BUSY`, nothing prints, **and** the light is blinking, it's holding a stuck job. Turn it off, wait 10 seconds and turn it back on.
2. **Remove hubs from the path.** On the original Mac, the printer sat behind two stacked hubs. Plug it straight into the Mac to rule them out.
3. **Check that the queue is enabled.** In `lpstat -p HP_Laser_108a`, an "enabled since" time that changes on its own means CUPS stopped the queue after a backend error and something restarted it. Run `cupsenable HP_Laser_108a`.
4. **Recreate the queue.** In System Settings, remove the printer and add it again as described above. The macOS upgrade can leave a stale queue behind.
5. **Reinstall the Samsung driver** if the `file` check above shows only `x86_64` or the file is missing.

### Detailed logs

```sh
sudo cupsctl --debug-logging      # turn on
lp -d HP_Laser_108a /etc/hosts
tail -f /var/log/cups/error_log    # watch the job
sudo cupsctl --no-debug-logging   # turn off afterwards
```

### Test the driver without the printer

This runs the full driver pipeline into a file. If the file starts with `@PJL … ENTER LANGUAGE=QPDL`, the software side is working:

```sh
cupsfilter -p /etc/cups/ppd/HP_Laser_108a.ppd -m printer/foo -e /etc/hosts > out.prn
head -c 400 out.prn | strings
```

To send that file to the printer as-is, bypassing the driver:

```sh
lp -d HP_Laser_108a -o raw out.prn
```

### Other symptoms

| Symptom | Likely cause |
|---|---|
| Printer missing from the Add list | Cable, power, or a hub. Check `lpinfo -v`. |
| `Bad CPU type` / status 86 in `error_log` | An Intel-only driver on a Mac without Rosetta. Install v3.93 or later. |
| "Filter failed" | The Samsung driver is missing. The queue still points to `/Library/Printers/Samsung/UPD/Filters/rastertosec`. |
| Prints garbage text | The wrong driver was picked (Generic PCL/PS). Re-add the printer with **Samsung M2020 Series**. |

## Sharing it with other Macs and devices

Turn on **System Settings → General → Sharing → Printer Sharing** and tick the HP Laser 108a. Other Macs can then print through this Mac without installing the driver themselves. The host Mac has to be awake.

If no Mac is always on, a Raspberry Pi running CUPS with the SpliX driver and AirPrint sharing works as a small always-on print server.
