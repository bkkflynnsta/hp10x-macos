#!/bin/sh
# Uninstall the hp10x driver. Run with: sudo /Library/Printers/hp10x/uninstall.sh
# Printer queues that use the driver are left in place; delete them in System Settings.
set -e
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

/bin/launchctl bootout system/com.pointcleardigital.hp10x.ippusb-guard 2>/dev/null || true

# Hand IPP-over-USB back to macOS for the printers the guard disabled.
/bin/launchctl print-disabled system 2>/dev/null |
    sed -n 's/^[[:space:]]*"\(com\.apple\.print\.ippusb\.[^"]*HP Laser 10[378][^"]*\)" => disabled.*/\1/p' |
    while IFS= read -r label; do
        /bin/launchctl enable "system/$label" && echo "re-enabled $label"
    done

rm -f /Library/LaunchDaemons/com.pointcleardigital.hp10x.ippusb-guard.plist
rm -f "/Library/Printers/PPDs/Contents/Resources/HP Laser 10x (hp10x).gz"
rm -rf /Library/Printers/hp10x
/usr/sbin/pkgutil --forget com.pointcleardigital.hp10x >/dev/null 2>&1 || true
/bin/launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true
echo "hp10x removed."
