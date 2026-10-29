#!/bin/bash
# Install the NS7 driver: ns7d (service + MIDI helper), the NS7 Audio plug-in
# and their launchd jobs. No kernel extension, SIP stays on.
#   ./install.sh            build and install
#   ./install.sh --no-build install what's already built
set -euo pipefail
cd "$(dirname "$0")"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname)" == Darwin ]] || die "this installs on macOS"

BUILD=1
[[ "${1:-}" == "--no-build" ]] && BUILD=0

if (( BUILD )); then
    say "Checking build tools"
    xcode-select -p >/dev/null 2>&1 || die "install the Xcode command line tools first: xcode-select --install"
    command -v brew >/dev/null || die "Homebrew is needed (https://brew.sh) for libusb and cmake"
    for pkg in libusb pkg-config cmake; do
        brew list --formula "$pkg" >/dev/null 2>&1 || brew install "$pkg"
    done

    say "Building ns7d"
    make clean >/dev/null
    make
    say "Running the unit tests"
    make test

    say "Building the NS7 Audio plug-in (fetches libASPL the first time)"
    cmake -S plugin -B build/plugin -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build/plugin --config Release
fi

[[ -x ns7d ]] || die "ns7d isn't built"
[[ -d build/plugin/NS7Audio.driver ]] || die "the plug-in isn't built"

say "Installing (you'll be asked for your password)"
sudo launchctl bootout system/com.ns7driver.daemon 2>/dev/null || true
launchctl bootout "gui/$(id -u)/com.ns7driver.midi" 2>/dev/null || true

sudo install -d /usr/local/bin
sudo install -m 755 ns7d /usr/local/bin/ns7d
sudo rm -rf /Library/Audio/Plug-Ins/HAL/NS7Audio.driver
sudo cp -R build/plugin/NS7Audio.driver /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/NS7Audio.driver
sudo install -m 644 -o root -g wheel macos/com.ns7driver.daemon.plist /Library/LaunchDaemons/
sudo install -m 644 -o root -g wheel macos/com.ns7driver.midi.plist /Library/LaunchAgents/

say "Starting the driver service and the MIDI helper"
sudo launchctl bootstrap system /Library/LaunchDaemons/com.ns7driver.daemon.plist
launchctl bootstrap "gui/$(id -u)" /Library/LaunchAgents/com.ns7driver.midi.plist

say "Restarting CoreAudio so it loads the plug-in"
sudo killall coreaudiod 2>/dev/null || true

cat <<'EOF'

Installed. Plug in the NS7 (and switch it on). Within a few seconds:
  - "Numark NS7" appears in Audio MIDI Setup and in your DJ app's audio settings
  - an "NS7" MIDI input and output appear for controller mapping

If macOS asks whether to allow the accessory to connect, choose Allow.
Service log:  /Library/Logs/ns7d.log      MIDI helper log: /tmp/ns7-midi.log
Quick checks: ns7d info  (with the service stopped: sudo launchctl bootout system/com.ns7driver.daemon)
Uninstall:    ./uninstall.sh
EOF
