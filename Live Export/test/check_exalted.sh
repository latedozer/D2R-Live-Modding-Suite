#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Edits to Exalted Items' own cube rows, exported both ways, through Exalted Items' own generator
# (agreed with the Exalted Items session, Exalted Items 1.2.0 or later):
#   1. its generator on the game's tables -> its cubemain (what the user's list is made against)
#   2. exalted_test: a list with 3 Exalted recipes changed, 1 removed -> mod.ts + plain cubemain
#   3. the D2RMM path: mod.ts on the game's tables (stand-in D2RMM), then the generator on that
#   4. the plain .mpq path: the plain cubemain, then the generator on that
#   Both must keep the 3 changed recipes as they are, the removed one with enabled 0, and make
#   all the others themselves.
# EXALTED=<exalted-plugin folder> VANILLA=<game data folder> CODES=<codes.txt> ./test/check_exalted.sh
set -e
cd "$(dirname "$0")/.."
EXALTED="${EXALTED:-../../exalted mod/exalted-plugin}"
VANILLA="${VANILLA:-../../vanilla/data}"
CODES="${CODES:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected/d2rloader/exalted-items/D2RMM/codes.txt}"
GEN="$EXALTED/build/native/gen_test"
[ -x "$GEN" ] || { echo "Exalted Items' generator not built: $GEN (see its test folder)"; exit 1; }
[ -f "$CODES" ] || { echo "codes.txt not found: $CODES"; exit 1; }
W=build/xlt
rm -rf "$W" && mkdir -p "$W/plain/global/excel"
status=0
"$GEN" "$VANILLA" "$VANILLA" "$W/base" crystal=1 codes="$CODES" > "$W/base.log"
g++ -std=c++20 -O2 -Wall -Wextra -o build/exalted_test test/exalted_test.cpp export_core.cpp
./build/exalted_test "$W/base/global/excel/cubemain.txt" "$VANILLA/global/excel/cubemain.txt" "$W" || status=1
node test/merge_d2rmm.mjs "$VANILLA" "$W/mod.ts" "$W/merged"
cp "$W/plain-cubemain.txt" "$W/plain/global/excel/cubemain.txt"
for way in merged plain; do
	"$GEN" "$W/$way" "$VANILLA" "$W/$way-out" crystal=1 codes="$CODES" > "$W/$way.log"
	node test/check_exalted_out.mjs "$W/$way-out/global/excel/cubemain.txt" "$W/expect.txt" "$W/$way.log" "$way" || status=1
done
exit $status
