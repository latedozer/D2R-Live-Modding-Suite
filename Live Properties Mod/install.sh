#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-live-properties.dll
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
	rm -f "$DEST/d2rl-live-properties.dll"
	echo "removed. Settings (config/live-properties.toml) were left in $GAME/d2rloader"
	exit 0
fi
[ -f build/d2rl-live-properties.dll ] || { echo "build it first: ./build.sh"; exit 1; }
VERSION="$(sed -n 's/^constexpr const char\* kVersion = "\(.*\)";/\1/p' plugin.cpp)"
STAMP="$(date +%Y%m%d-%H%M%S)"
NEW_SHA="$(sha256sum build/d2rl-live-properties.dll | cut -d' ' -f1)"
if [ -f "$DEST/d2rl-live-properties.dll" ]; then
	# the DLL it replaces is kept, so it can be put back
	PREV="../Suite .dlls/previous"
	mkdir -p "$PREV"
	OLD_SHA="$(sha256sum "$DEST/d2rl-live-properties.dll" | cut -d' ' -f1)"
	cp "$DEST/d2rl-live-properties.dll" "$PREV/d2rl-live-properties.replaced-$STAMP-${OLD_SHA:0:8}.dll"
	echo "kept the old one as $PREV/d2rl-live-properties.replaced-$STAMP-${OLD_SHA:0:8}.dll"
fi
cp build/d2rl-live-properties.dll "$DEST/d2rl-live-properties.dll.installing"
mv "$DEST/d2rl-live-properties.dll.installing" "$DEST/d2rl-live-properties.dll"
mkdir -p "$GAME/d2rloader/live-suite"
printf '%s\tlive-properties\t%s\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$VERSION" "$NEW_SHA" >> "$GAME/d2rloader/live-suite/installed.txt"
ls -la "$DEST/d2rl-live-properties.dll"
echo "installed $VERSION (sha256 $NEW_SHA); noted in d2rloader/live-suite/installed.txt"
echo "installed. Start the game, enter a game, open the Mod Book (Ctrl+B) or type prop in the console"
