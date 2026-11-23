#!/bin/bash
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-live-export.dll
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
	rm -f "$DEST/d2rl-live-export.dll"
	echo "removed. Settings (config/live-export.toml) were left in $GAME/d2rloader"
	exit 0
fi
[ -f build/d2rl-live-export.dll ] || { echo "build it first: ./build.sh"; exit 1; }
cp build/d2rl-live-export.dll "$DEST/d2rl-live-export.dll.installing"
mv "$DEST/d2rl-live-export.dll.installing" "$DEST/d2rl-live-export.dll"
ls -la "$DEST/d2rl-live-export.dll"
echo "installed."
