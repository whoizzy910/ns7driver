#!/bin/bash
# Remove everything install.sh put in place.
set -uo pipefail
echo "==> Removing the NS7 driver (you'll be asked for your password)"
sudo launchctl bootout system/com.ns7driver.daemon 2>/dev/null
launchctl bootout "gui/$(id -u)/com.ns7driver.midi" 2>/dev/null
sudo rm -f /Library/LaunchDaemons/com.ns7driver.daemon.plist /Library/LaunchAgents/com.ns7driver.midi.plist
sudo rm -rf /Library/Audio/Plug-Ins/HAL/NS7Audio.driver
sudo rm -f /usr/local/bin/ns7d
sudo killall coreaudiod 2>/dev/null
echo "Done. Logs are left in /Library/Logs/ns7d.log and /tmp/ns7-midi.log."
