#!/bin/bash
# Double-click to install the NS7 driver. Installs what it needs (Apple's
# command line tools, Homebrew, libusb, cmake), builds, tests and installs.
# You'll be asked for your Mac password; that's normal.

cd "$(dirname "$0")" || exit 1
LOG="$HOME/Library/Logs/ns7driver-install.log"
mkdir -p "$(dirname "$LOG")"
exec > >(tee "$LOG") 2>&1

bold() { printf '\n\033[1m%s\033[0m\n' "$*"; }
finish() {
    echo
    echo "A log of this install is in $LOG"
    read -n 1 -s -r -p "Press any key to close this window."
    echo
    exit "$1"
}
fail() {
    printf '\n\033[31mInstall stopped: %s\033[0m\n' "$*"
    echo "Send the log file to get it fixed."
    finish 1
}

clear
bold "Numark NS7 driver installer"
echo "This takes a few minutes the first time. Keep this window open."

# 1. Apple's command line tools (compiler, git)
if ! xcode-select -p >/dev/null 2>&1; then
    bold "Installing Apple's command line tools"
    echo "A window will pop up: click Install, then wait for it to finish."
    xcode-select --install >/dev/null 2>&1
    until xcode-select -p >/dev/null 2>&1; do sleep 5; done
fi

# 2. Homebrew (for libusb, pkg-config and cmake)
if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
fi
if ! command -v brew >/dev/null; then
    bold "Installing Homebrew"
    echo "Enter your Mac password when asked (nothing shows as you type)."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" \
        || fail "Homebrew didn't install"
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    else
        eval "$(/usr/local/bin/brew shellenv)"
    fi
fi

# 3. Build, test and install
bold "Building and installing the driver"
echo "Enter your Mac password if asked."
./install.sh || fail "the build or install failed (see above)"

bold "Done!"
echo "Plug in your NS7 and switch it on. In a few seconds \"Numark NS7\""
echo "shows up as an audio device, with \"NS7\" MIDI ports for mapping."
echo "If macOS asks to allow the accessory to connect, click Allow."
finish 0
