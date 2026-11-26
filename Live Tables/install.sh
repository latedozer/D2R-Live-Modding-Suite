#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-live-tables.dll
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
	rm -f "$DEST/d2rl-live-tables.dll"
	echo "removed. Settings (config/live-tables.toml) were left in $GAME/d2rloader"
	exit 0
fi
[ -f build/d2rl-live-tables.dll ] || { echo "build it first: ./build.sh"; exit 1; }
# the build now installed is kept (with its version) before it is replaced, and every install is recorded
NEW_VERSION=$(sed -n 's/^constexpr const char\* kVersion = "\(.*\)";/\1/p' plugin.cpp)
NEW_SHA=$(sha256sum build/d2rl-live-tables.dll | cut -d' ' -f1)
LEDGER="$GAME/d2rloader/live-suite/installed.txt"
mkdir -p "$(dirname "$LEDGER")"
if [ -f "$DEST/d2rl-live-tables.dll" ]; then
	OLD_SHA=$(sha256sum "$DEST/d2rl-live-tables.dll" | cut -d' ' -f1)
	OLD_VERSION=$(grep -a "	d2rl-live-tables.dll	" "$LEDGER" 2>/dev/null | awk -F'\t' -v s="$OLD_SHA" '$4==s{v=$3} END{print v}')
	[ -n "$OLD_VERSION" ] || OLD_VERSION="unknown-${OLD_SHA:0:12}"
	mkdir -p "../Suite .dlls/previous"
	cp "$DEST/d2rl-live-tables.dll" "../Suite .dlls/previous/d2rl-live-tables.$OLD_VERSION.dll"
	echo "kept the build that was installed: Suite .dlls/previous/d2rl-live-tables.$OLD_VERSION.dll"
fi
cp build/d2rl-live-tables.dll "$DEST/d2rl-live-tables.dll.installing"
mv "$DEST/d2rl-live-tables.dll.installing" "$DEST/d2rl-live-tables.dll"
ls -la "$DEST/d2rl-live-tables.dll"
printf '%s\t%s\t%s\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "d2rl-live-tables.dll" "$NEW_VERSION" "$NEW_SHA" >> "$LEDGER"
echo "recorded in $LEDGER: Live Tables $NEW_VERSION $NEW_SHA"
echo "installed. Start the game and type tables in the console"
