#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Builds everything and runs the checks:
#   1. cube_core on its own (natively): the game's own cubemain files, and the list builder
#   2. the plugin's addresses against the dump of the game image (../bin/D2RLoader_6ab3782c_dump.exe)
#   3. the real plugin DLL under Wine, in a stand-in for the game and the loader
# The game's own files are read from ../vanilla/data/global/excel (cubemain.txt/.bin,
# Properties.txt, PlayerClass.txt and the same under base/); set EXCEL=... for another place.
set -e
cd "$(dirname "$0")/.."
SDK="${SDK:-../PluginSDK-master/include}"
EXCEL="${EXCEL:-$PWD/../vanilla/data/global/excel}"
./build.sh > /dev/null
status=0
echo "== core"
g++ -std=c++20 -O2 -Wall -Wextra -o build/core_test test/core_test.cpp cube_core.cpp
./build/core_test "$EXCEL" | grep -E '^(ok|FAIL|skip|ALL|FAILURES|      )' || status=1
[ "${PIPESTATUS[0]}" = 0 ] || status=1
echo "== addresses"
python3 test/check_prints.py plugin.cpp "${IMAGE:-../bin/D2RLoader_6ab3782c_dump.exe}" || status=1
echo "== plugin under Wine"
x86_64-w64-mingw32-g++ -std=c++20 -O2 -shared -static -static-libgcc -static-libstdc++ -o build/D2RCore.dll test/fake_core.cpp
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wno-missing-field-initializers -static -static-libgcc -static-libstdc++ -I "$SDK" -o build/fake_host.exe test/fake_host.cpp
WORK="$(mktemp -d)"
( cd "../Mod Book" && ./build.sh > /dev/null )
( cd build && WINEDEBUG=-all timeout 300 wine fake_host.exe "$(winepath -w "$PWD/d2rl-live-cube-recipes.dll")" "$(winepath -w "$WORK")" "$(winepath -w "$EXCEL")" "$(winepath -w "$PWD/../../Mod Book/build/d2rl-mod-book.dll")" 2>/dev/null ) > build/fake_host.out || status=1
grep -E '^(ok|FAIL|ALL|FAILURES)' build/fake_host.out || status=1
grep -q '^ALL PASSED' build/fake_host.out || status=1
python3 - "$WORK/RecipeBookhd.json" <<'PY' || status=1
import json, sys
try:
    layout = json.load(open(sys.argv[1]))
    names = []
    def walk(node):
        names.append(node["name"])
        for child in node.get("children", []):
            walk(child)
    walk(layout)
    ok = layout["type"] == "Panel" and layout["name"].startswith("mod-book/Left") and len(names) == len(set(names)) and "Title7" in names and "Detail7" in names
    print(("ok  : " if ok else "FAIL: ") + "the panel layout is strict JSON, rooted at a Panel with the Mod Book's name, %d widgets with names of their own" % len(names))
    sys.exit(0 if ok else 1)
except Exception as error:
    print("FAIL: the panel layout is not valid JSON:", error)
    sys.exit(1)
PY
rm -rf "$WORK"
[ $status = 0 ] && echo "== all checks passed" || echo "== FAILED (full output: build/fake_host.out)"
exit $status
