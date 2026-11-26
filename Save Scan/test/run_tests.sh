#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# 1. the save reader natively, against COPIES of the saves (made here, into build/; the live
#    files are only copied from) and the mod's tables as text
# 2. the save writer natively, on copies of those copies (build/fix-work, removed after)
# 3. the plugin's addresses against the dump of the game image
# The plugin itself (plugin.cpp) is only built here; it has no stand-in host.
#   SAVES=/folder/with/saves  EXCEL=/folder/with/the/txt/tables  IMAGE=/path/to/dump  ./test/run_tests.sh
set -e
cd "$(dirname "$0")/.."
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
SAVES="${SAVES:-$HOME/FI/SteamLibrary/steamapps/compatdata/2536520/pfx/drive_c/users/steamuser/Saved Games/Diablo II Resurrected/mods/D2RMM}"
EXCEL="${EXCEL:-$GAME/d2rloader/exalted-items/D2RMM/data/global/excel}"
GROWN="${GROWN:-$GAME/d2rloader/live-suite/live-tables/D2RMM/grown-rows.txt}" # Live Tables' made rows (the game has them through its hook)
EDITS="${EDITS:-$GAME/d2rloader/live-suite/live-properties/D2RMM/stat-edits.txt}" # Live Properties' stat edits (the game has them in memory)
./build.sh > /dev/null
status=0
echo "== core"
rm -rf build/saves-copy && mkdir -p build/saves-copy
cp "$SAVES"/*.d2s "$SAVES"/*.d2i build/saves-copy/ 2>/dev/null || true
rm -f build/grown-rows.txt build/stat-edits.txt; [ -f "$GROWN" ] && cp "$GROWN" build/grown-rows.txt; [ -f "$EDITS" ] && cp "$EDITS" build/stat-edits.txt
g++ -std=c++20 -O2 -Wall -Wextra -o build/core_test test/core_test.cpp save_core.cpp fix_core.cpp
./build/core_test build/saves-copy "$EXCEL" build/grown-rows.txt build/stat-edits.txt || status=1
echo "== writer"
g++ -std=c++20 -O2 -Wall -Wextra -o build/fix_test test/fix_test.cpp fix_core.cpp save_core.cpp
./build/fix_test build/saves-copy "$EXCEL" build/fix-work build/grown-rows.txt build/stat-edits.txt || status=1
echo "== addresses"
python3 test/check_prints.py plugin.cpp "${IMAGE:-../bin/D2RLoader_6ab3782c_dump.exe}" || status=1
exit $status
