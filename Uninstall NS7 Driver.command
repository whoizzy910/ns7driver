#!/bin/bash
# Double-click to remove the NS7 driver.
cd "$(dirname "$0")" || exit 1
./uninstall.sh
echo
read -n 1 -s -r -p "Press any key to close this window."
echo
