#!/bin/sh
# Remove all HP Laser 108a support: the hp10x driver and HP's Samsung printer driver.
# Use it for a clean reinstall. Print queues are left alone; delete them first in
# System Settings, or with lpadmin -x.
# Run with: sudo scripts/remove-108a-support.sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

# hp10x: stops the guard, re-enables macOS's IPP-over-USB for the 108a, removes its files.
if [ -x /Library/Printers/hp10x/uninstall.sh ]; then
    /Library/Printers/hp10x/uninstall.sh
fi

# HP's Samsung driver 3.93 (package com.samsung.PrinterDriver): remove exactly the PPDs
# it installed, then its own folders and cache.
if /usr/sbin/pkgutil --pkg-info com.samsung.PrinterDriver >/dev/null 2>&1; then
    /usr/sbin/pkgutil --files com.samsung.PrinterDriver |
        grep '^Library/Printers/PPDs/Contents/Resources/Samsung .*\.gz$' |
        while IFS= read -r f; do rm -f "/$f"; done
    rm -rf /Library/Printers/Samsung "/Library/Application Support/Samsung"
    rm -f /Library/Caches/com.sec.printer
    /usr/sbin/pkgutil --forget com.samsung.PrinterDriver
    echo "Samsung driver removed."
fi

/bin/launchctl kickstart -k system/org.cups.cupsd 2>/dev/null
echo "Done. Unplug the printer's USB cable, then plug it back in before reinstalling."
