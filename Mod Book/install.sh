#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-mod-book.dll
#   ./install.sh remove   take the plugin out (its settings stay)
#
# Refuses while the game is running. GAME=/path/to/Diablo\ II\ Resurrected overrides the game folder.
set -e
cd "$(dirname "$0")"
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
DEST="$GAME/d2rloader/plugins"
[ -d "$DEST" ] || { echo "plugin folder not found: $DEST"; exit 1; }
if ps -eo args | grep -q "[D]2R\(Loader\)\?[.]exe"; then
	echo "The game is running. Close it, then run this again."
	exit 1
fi
if [ "${1:-}" = "remove" ]; then
	rm -f "$DEST/d2rl-mod-book.dll"
	echo "removed. Settings (config/mod-book.toml) were left in $GAME/d2rloader"
	exit 0
fi
[ -f build/d2rl-mod-book.dll ] || { echo "build it first: ./build.sh"; exit 1; }
cp build/d2rl-mod-book.dll "$DEST/d2rl-mod-book.dll.installing"
mv "$DEST/d2rl-mod-book.dll.installing" "$DEST/d2rl-mod-book.dll"
ls -la "$DEST/d2rl-mod-book.dll"
echo "installed."
