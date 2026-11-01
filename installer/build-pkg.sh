#!/bin/bash
# build-pkg.sh - build build/hp10x-<version>.pkg
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Environment:
#   SIGN_ID       codesign identity for the binaries (default: the pointcleardigital.com
#                 "Apple Development" one; use "Developer ID Application: ..." for releases)
#   INSTALLER_ID  productbuild identity ("Developer ID Installer: ..."); unsigned if unset
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^\*FileVersion: "\(.*\)"/\1/p' "$ROOT/ppd/HP-Laser-10x.ppd")
BUILD=$ROOT/build
STAGE=$BUILD/pkgroot
ID=com.pointcleardigital.hp10x

SIGN_ID=${SIGN_ID:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development:[^"]*pointcleardigital[^"]*\)".*/\1/p' | head -1)}

make -C "$ROOT" all

rm -rf "$STAGE"
install -d "$STAGE/Library/Printers/hp10x/Filters" \
           "$STAGE/Library/Printers/PPDs/Contents/Resources" \
           "$STAGE/Library/LaunchDaemons"
install -m 755 "$BUILD/rastertohp10x"        "$STAGE/Library/Printers/hp10x/Filters/"
install -m 755 "$BUILD/hp10x-ippusb-guard"   "$STAGE/Library/Printers/hp10x/"
install -m 755 "$ROOT/installer/uninstall.sh" "$STAGE/Library/Printers/hp10x/"
install -m 644 "$ROOT/LICENSE"               "$STAGE/Library/Printers/hp10x/"
install -m 644 "$ROOT/installer/com.pointcleardigital.hp10x.ippusb-guard.plist" "$STAGE/Library/LaunchDaemons/"
gzip -9 -n -c "$ROOT/ppd/HP-Laser-10x.ppd" > "$STAGE/Library/Printers/PPDs/Contents/Resources/HP Laser 10x (hp10x).gz"
chmod 644 "$STAGE/Library/Printers/PPDs/Contents/Resources/HP Laser 10x (hp10x).gz"

if [[ -n $SIGN_ID ]]; then
    for f in "$STAGE/Library/Printers/hp10x/Filters/rastertohp10x" "$STAGE/Library/Printers/hp10x/hp10x-ippusb-guard"; do
        codesign --force --options runtime --timestamp=none --identifier "$ID.$(basename "$f")" --sign "$SIGN_ID" "$f"
    done
    echo "signed binaries with: $SIGN_ID"
else
    echo "warning: no signing identity found; binaries are unsigned" >&2
fi

pkgbuild --root "$STAGE" --identifier "$ID" --version "$VERSION" \
         --scripts "$ROOT/installer/scripts" --ownership recommended \
         "$BUILD/hp10x-component.pkg"

PRODUCT_ARGS=()
[[ -n ${INSTALLER_ID:-} ]] && PRODUCT_ARGS=(--sign "$INSTALLER_ID")
productbuild --distribution "$ROOT/installer/distribution.xml" --package-path "$BUILD" \
             --resources "$ROOT/installer/resources" ${PRODUCT_ARGS[@]+"${PRODUCT_ARGS[@]}"} \
             "$BUILD/hp10x-$VERSION.pkg"
rm -f "$BUILD/hp10x-component.pkg"
echo "built $BUILD/hp10x-$VERSION.pkg"
