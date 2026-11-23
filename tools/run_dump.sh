#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Double-click (choose "Run in Konsole"/"Execute") while D2R is running through D2RLoader.
# A password window pops up; the dump takes a few seconds.
DIR="$(cd "$(dirname "$0")" && pwd)"
pkexec /usr/bin/python3 "$DIR/dump_image_linux.py"
echo
echo "Done. Result is in tools/dump.log - you can close this window."
read -r -p "Press Enter to close..."
