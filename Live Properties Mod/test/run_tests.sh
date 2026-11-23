#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# 1. prop_core natively, against a port of the game's own linking routine
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
echo "== addresses"
python3 test/check_prints.py plugin.cpp "${IMAGE:-../bin/D2RLoader_6ab3782c_dump.exe}" || status=1
exit $status
