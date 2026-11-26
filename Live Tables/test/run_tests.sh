#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# 1. the game's own name hash and lower-case routines, run from the image dump, against the port
# 2. copies of the tables grown in the model == the same tables loaded with the rows in the text
# 3. the read hook's name scan, made-up cases and the mod's own other tables (copies)
# 4. D2RCore's stat-name list (MSVC layout) grown by a name, on copies made as D2RCore makes them
# 5. the bytes plugin.cpp expects in the game and in D2RCore.dll
# Only copies in build/tables are read by the test; the mod folder and the game folder are only copied from.
set -e
cd "$(dirname "$0")/.."
./build.sh > /dev/null
status=0
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
VAN=../vanilla/data/global/excel
MOD="$GAME/mods/D2RMM/D2RMM.mpq/data/global/excel"
EXA="$GAME/d2rloader/exalted-items/D2RMM/data/global/excel"
rm -rf build/tables; mkdir -p build/tables/vanilla build/tables/mod build/tables/mod-base build/tables/exalted
copy() { # copy() <from folder> <to folder>: itemstatcost + properties, any case
  local i p
  i=$(find "$1" -maxdepth 1 -iname itemstatcost.txt 2>/dev/null | head -1)
  p=$(find "$1" -maxdepth 1 -iname properties.txt 2>/dev/null | head -1)
  [ -n "$i" ] && [ -n "$p" ] && cp "$i" "$2/itemstatcost.txt" && cp "$p" "$2/properties.txt"
}
sets=()
copy "$VAN" build/tables/vanilla && sets+=(vanilla)
copy "$MOD" build/tables/mod && sets+=(mod)
copy "$MOD/base" build/tables/mod-base && sets+=(mod-base)
copy "$EXA" build/tables/exalted && sets+=(exalted)
echo "== the game's hash"
names=()
for s in "${sets[@]}"; do names+=("build/tables/$s/itemstatcost.txt" "build/tables/$s/properties.txt"); done
./build/grow_test hash "${DUMP:-../bin/D2RLoader_6ab3782c_dump.exe}" "${names[@]}" || status=1
for s in "${sets[@]}"; do
  echo "== grow: $s"
  ./build/grow_test grow "build/tables/$s/itemstatcost.txt" "build/tables/$s/properties.txt" "$s" || status=1
done
for s in "${sets[@]}"; do
  echo "== D2RCore stat-name list: $s"
  ./build/name_list_test "build/tables/$s/itemstatcost.txt" || status=1
done
echo "== kept numbers per data version"
./build/grow_test kept || status=1
echo "== the read hook's name scan"
others=()
for f in skills states misc weapons armor monstats properties uniqueitems setitems cubemain magicprefix magicsuffix; do
  g=$(find "$MOD" -maxdepth 1 -iname "$f.txt" 2>/dev/null | head -1); [ -n "$g" ] && cp "$g" "build/tables/other-$f.txt" && others+=("build/tables/other-$f.txt")
done
./build/names_test build/tables/mod/itemstatcost.txt "${others[@]}" || status=1
echo "== the game's code the plugin relies on"
python3 test/check_prints.py plugin.cpp "${DUMP:-../bin/D2RLoader_6ab3782c_dump.exe}" "${CORE:-$GAME/D2RCore.dll}" || status=1
[ $status = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $status
