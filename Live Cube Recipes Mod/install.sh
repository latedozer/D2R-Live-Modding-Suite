#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-live-cube-recipes.dll
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
	rm -f "$DEST/d2rl-live-cube-recipes.dll"
	echo "removed. Settings (config/live-cube-recipes.toml) were left in $GAME/d2rloader"
	exit 0
fi
NAME=d2rl-live-cube-recipes
NEW="build/$NAME.dll"
OLD="$DEST/$NAME.dll"
PREVIOUS="${PREVIOUS:-../Suite .dlls/previous}" # (PREVIOUS=... for a test)
LOG="$GAME/d2rloader/live-suite/installed.txt"
[ -f "$NEW" ] || { echo "build it first: ./build.sh"; exit 1; }
# A DLL's version: its LiveSuiteVersion export (0.16.6 and later), else the line of installed.txt
# that has its sha256, else "unknown-<first 8 of its sha256>".
version_of() {
	local sha found
	found="$(strings -a "$1" | sed -n "s/^$NAME \([0-9][0-9.]*\)\$/\1/p" | head -1)"
	if [ -z "$found" ]; then
		sha="$(sha256sum "$1" | cut -c1-64)"
		[ -f "$LOG" ] && found="$(awk -F'\t' -v n="$NAME.dll" -v s="$sha" '$2 == n && $4 == s { v = $3 } END { print v }' "$LOG")"
		[ -n "$found" ] || found="unknown-${sha:0:8}"
	fi
	echo "$found"
}
NEW_VERSION="$(version_of "$NEW")"
NEW_SHA="$(sha256sum "$NEW" | cut -c1-64)"
# the DLL that was installed goes to Suite .dlls/previous/<name>.<its version>.dll first
if [ -f "$OLD" ] && ! cmp -s "$OLD" "$NEW"; then
	mkdir -p "$PREVIOUS"
	KEPT="$PREVIOUS/$NAME.$(version_of "$OLD").dll"
	if [ -f "$KEPT" ] && ! cmp -s "$KEPT" "$OLD"; then
		KEPT="$PREVIOUS/$NAME.$(version_of "$OLD").$(sha256sum "$OLD" | cut -c1-8).dll"
	fi
	cp "$OLD" "$KEPT"
	echo "the DLL that was installed is kept as $KEPT"
fi
cp "$NEW" "$DEST/$NAME.dll.installing"
mv "$DEST/$NAME.dll.installing" "$OLD"
cmp -s "$NEW" "$OLD" || { echo "the installed DLL is not the build"; exit 1; }
mkdir -p "$(dirname "$LOG")"
printf '%s\t%s\t%s\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$NAME.dll" "$NEW_VERSION" "$NEW_SHA" >> "$LOG"
ls -la "$OLD"
echo "$NAME.dll $NEW_VERSION sha256 $NEW_SHA (noted in d2rloader/live-suite/installed.txt)"
echo "installed. Start the game once; your recipe file is then d2rloader/live-suite/live-cube-recipes/<mod>/recipes.txt (README.txt beside it)."
