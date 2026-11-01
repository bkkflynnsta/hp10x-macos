#!/bin/zsh
# Health check for an HP Laser 103/107/108 queue on macOS. Read-only; prints nothing to paper.
# SPDX-License-Identifier: GPL-2.0-or-later
Q=${1:-HP_Laser_108a}
F=/Library/Printers/Samsung/UPD/Filters/rastertosec
ok()   { print -nP "%F{green}OK%f   "; print -r -- "$*"; }
bad()  { print -nP "%F{red}FAIL%f "; print -r -- "$*"; }
warn() { print -nP "%F{yellow}WARN%f "; print -r -- "$*"; }

print "macOS $(sw_vers -productVersion) on $(uname -m)"

if [[ -x $F ]]; then
  archs=$(lipo -archs $F 2>/dev/null)
  [[ $archs == *$(uname -m)* ]] && ok "Samsung filter present ($archs)" || bad "Samsung filter lacks $(uname -m) ($archs)"
else
  bad "Samsung driver not installed ($F missing)"
fi

dev=$(/usr/libexec/cups/backend/usb 2>/dev/null | grep -i 'Laser%20103' | head -1)
if [[ -n $dev ]]; then
  ok "Printer on USB: $(print $dev | awk '{print $2}')"
  st=$(print $dev | sed -n 's/.*STATUS:\([A-Z]*\).*/\1/p')
  [[ $st == BUSY ]] && warn "Printer reports STATUS:BUSY (often harmless; power-cycle only if nothing prints and the light blinks)" || ok "Printer status: ${st:-unknown}"
  # Each "+-o" starts a USB interface entry; report owners of the printer's interfaces.
  owner=$(ioreg -r -c IOUSBHostInterface -l -w0 | awk -F'" = "' '
    /\+-o/ { if (hp && own) print own; hp = 0; own = "" }
    /"USB Product Name" = "HP Laser 10[378]/ { hp = 1 }
    /"UsbExclusiveOwner"/ { own = $2; sub(/"$/, "", own) }
    END { if (hp && own) print own }' | sort -u | paste -sd, -)
  launchctl print-disabled system 2>/dev/null | grep -q 'ippusb\.HP Inc\.\.HP Laser 10[378].*=> disabled' \
    && ok "ippusbd launchd job disabled for this printer" \
    || warn "ippusbd launchd job not disabled (see README: Permanent fix)"
  [[ $owner == *ippusbd* ]] && bad "USB interface held by ippusbd ($owner); run: sudo killall ippusbd, then cancel -a $Q and reprint" || ok "USB interface not held by ippusbd"
else
  bad "Printer not detected on USB (check cable, power, hubs)"
fi

if lpstat -p $Q >/dev/null 2>&1; then
  ok "Queue $Q exists: $(lpstat -p $Q | head -1)"
  grep -q 'M2020' /etc/cups/ppd/$Q.ppd 2>/dev/null && ok "Queue uses Samsung M2020 Series PPD" || bad "Queue is not using the Samsung M2020 Series driver"
  lpstat -v $Q | grep -q 'Laser%20103' && ok "Queue points at the USB printer" || warn "Queue URI: $(lpstat -v $Q)"
else
  bad "Queue $Q not found (lpstat -p to list queues)"
fi

tmp=$(mktemp -d)
print "test" > $tmp/t.txt
if cupsfilter -p /etc/cups/ppd/$Q.ppd -m printer/foo -e $tmp/t.txt 2>/dev/null | head -c 600 | grep -aq 'LANGUAGE=QPDL'; then
  ok "Driver pipeline produces SPL/QPDL"
else
  bad "Driver pipeline did not produce SPL output"
fi
rm -rf $tmp

grep -E '^E ' /var/log/cups/error_log 2>/dev/null | grep -E "$Q|rastertosec|commandtosec|usb" | tail -3 | sed 's/^/      log: /'
