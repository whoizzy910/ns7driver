#!/bin/bash
# Build NS7Driver.pkg: a self-contained installer (no Homebrew needed on the
# target Mac; libusb is linked in statically). Runs on macOS, locally or in CI.
#   macos/make-pkg.sh            -> build/NS7Driver-<version>.pkg
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.1.0}"
LIBUSB_VERSION=1.0.27
OUT=build
STAGE=$OUT/pkgroot
mkdir -p "$OUT"

echo "==> libusb $LIBUSB_VERSION (static)"
if [[ ! -f $OUT/libusb/lib/libusb-1.0.a ]]; then
    curl -fsSL -o "$OUT/libusb.tar.bz2" \
        "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VERSION/libusb-$LIBUSB_VERSION.tar.bz2"
    rm -rf "$OUT/libusb-src" && mkdir -p "$OUT/libusb-src"
    tar -xjf "$OUT/libusb.tar.bz2" -C "$OUT/libusb-src" --strip-components 1
    (cd "$OUT/libusb-src" &&
        ./configure --prefix="$PWD/../libusb" --disable-shared --enable-static \
            CFLAGS="-O2 -mmacosx-version-min=11.0" >/dev/null &&
        make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null)
fi
LIBUSB_CFLAGS="-I$PWD/$OUT/libusb/include/libusb-1.0"
LIBUSB_LIBS="$PWD/$OUT/libusb/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security"

echo "==> ns7d"
make clean >/dev/null
make LIBUSB_CFLAGS="$LIBUSB_CFLAGS" LIBUSB_LIBS="$LIBUSB_LIBS" CFLAGS_EXTRA="-mmacosx-version-min=11.0"
make test LIBUSB_CFLAGS="$LIBUSB_CFLAGS" LIBUSB_LIBS="$LIBUSB_LIBS"
codesign --force --sign - ns7d

echo "==> NS7 Audio plug-in"
cmake -S plugin -B "$OUT/plugin" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$OUT/plugin" --config Release

echo "==> package"
rm -rf "$STAGE" && mkdir -p "$STAGE/usr/local/bin" "$STAGE/usr/local/share/ns7driver" \
    "$STAGE/Library/Audio/Plug-Ins/HAL" "$STAGE/Library/LaunchDaemons" "$STAGE/Library/LaunchAgents"
cp ns7d "$STAGE/usr/local/bin/"
cp -R "$OUT/plugin/NS7Audio.driver" "$STAGE/Library/Audio/Plug-Ins/HAL/"
cp macos/com.ns7driver.daemon.plist "$STAGE/Library/LaunchDaemons/"
cp macos/com.ns7driver.midi.plist "$STAGE/Library/LaunchAgents/"
cp uninstall.sh README.md LICENSE "$STAGE/usr/local/share/ns7driver/"
chmod 644 "$STAGE"/Library/Launch*/*.plist

pkgbuild --root "$STAGE" --scripts macos/pkg-scripts \
    --identifier com.ns7driver.pkg --version "$VERSION" --install-location / \
    "$OUT/NS7Driver-component.pkg"
productbuild --distribution macos/distribution.xml --package-path "$OUT" \
    --resources macos/pkg-resources "$OUT/NS7Driver-$VERSION.pkg"
rm -f "$OUT/NS7Driver-component.pkg"
echo "==> $OUT/NS7Driver-$VERSION.pkg"
