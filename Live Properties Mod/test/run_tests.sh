#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# 1. prop_core natively, against a port of the game's own linking routine; lt_core against a fake Live Tables
# 2. the plugin's addresses against the dump of the game image
# The plugin itself (plugin.cpp) is only built here; it has no stand-in host yet.
set -e
cd "$(dirname "$0")/.."
./build.sh > /dev/null
status=0
echo "== core"
g++ -std=c++20 -O2 -Wall -Wextra -o build/core_test test/core_test.cpp prop_core.cpp
./build/core_test || status=1
echo "== affixes"
g++ -std=c++20 -O2 -Wall -Wextra -o build/affix_test test/affix_test.cpp affix_core.cpp
./build/affix_test "${EXCEL:-../vanilla/data/global/excel/}" || status=1
echo "== legend"
g++ -std=c++20 -O2 -Wall -Wextra -o build/legend_test test/legend_test.cpp legend_core.cpp
./build/legend_test || status=1
echo "== op formulas (the stat card's game view)"
g++ -std=c++20 -O2 -Wall -Wextra -o build/op_test test/op_test.cpp op_core.cpp
./build/op_test || status=1
echo "== card typing (every typed cell stays pending until Save)"
g++ -std=c++20 -O2 -Wall -Wextra -o build/card_test test/card_test.cpp card_core.cpp
./build/card_test || status=1
echo "== Live Tables (a fake one: lt_core against live_tables_api.h)"
g++ -std=c++20 -O2 -Wall -Wextra -Itest/stub -o build/lt_test test/lt_test.cpp lt_core.cpp table_core.cpp
./build/lt_test test/stat-edits-2026-11-23.txt || status=1
echo "== no table read hook any more"
if grep -q '// d2rcore 0x' plugin.cpp || ! grep -q 'LivePropsUsesLiveTables = 1' plugin.cpp; then echo "FAIL  plugin.cpp still patches D2RCore, or does not export LivePropsUsesLiveTables"; status=1; else echo "ok    no D2RCore patch; LivePropsUsesLiveTables exported"; fi
echo "== saved affix ids (copies of the saves; Save Scan's reader; the D2RMM mod PerfectDrops run on copies)"
SAVES="${SAVES:-$HOME/FI/SteamLibrary/steamapps/compatdata/2536520/pfx/drive_c/users/steamuser/Saved Games/Diablo II Resurrected/mods/D2RMM}"
GAMEDIR="${GAMEDIR:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
PD="${PERFECTDROPS:-$HOME/D2RMM/mods/PerfectDrops}"
if [ -d "$SAVES" ] && [ -f "$PD/mod.js" ] && command -v node > /dev/null && [ -f "../Save Scan/save_core.cpp" ]; then
  rm -rf build/saves build/affix-tables && mkdir -p build/saves build/affix-tables/in build/affix-tables/out build/affix-tables/pre build/affix-tables/suf
  cp "$SAVES"/*.d2s "$SAVES"/*.d2i build/saves/ 2>/dev/null || true  # copies: the saves are only read
  cp ../vanilla/data/global/excel/MagicPrefix.txt build/affix-tables/in/magicprefix.txt
  cp "$GAMEDIR/mods/D2RMM/D2RMM.mpq/data/global/excel/magicsuffix.txt" build/affix-tables/in/magicsuffix.txt
  node test/run_d2rmm_mod.mjs "$PD" build/affix-tables/in build/affix-tables/out > /dev/null
  cp build/affix-tables/out/magicprefix.txt build/affix-tables/pre/MagicPrefix.txt && cp build/affix-tables/out/magicsuffix.txt build/affix-tables/suf/magicsuffix.txt
  g++ -std=c++20 -O2 -Wall -Wextra -o build/affix_ids_test test/affix_ids_test.cpp "../Save Scan/save_core.cpp"
  ./build/affix_ids_test build/saves "$GAMEDIR/d2rloader/exalted-items/D2RMM/data/global/excel" build/affix-tables/pre build/affix-tables/suf || status=1
else
  echo "SKIP  no saves, PerfectDrops, node or Save Scan source here"
fi
echo "== nothing written into the game's string memory"
python3 test/check_strings.py plugin.cpp || status=1
echo "== nothing reachable from done() throws"
python3 test/check_noexcept.py || status=1
echo "== button labels"
python3 test/check_labels.py plugin.cpp || status=1
echo "== addresses"
python3 test/check_prints.py plugin.cpp "${IMAGE:-../bin/D2RLoader_6ab3782c_dump.exe}" || status=1
exit $status
