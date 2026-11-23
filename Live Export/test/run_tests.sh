#!/bin/bash
# 1. export_core natively on the game's own tables (vanilla text): the edits into the tables
# 2. the D2RMM mod it writes, run by node against the same tables with a stand-in D2RMM, must
#    come out as the tables of step 1
# The plugin itself (plugin.cpp) is only built here.
set -e
cd "$(dirname "$0")/.."
EXCEL="${EXCEL:-../vanilla/data/global/excel}"
./build.sh > /dev/null
status=0
rm -rf build/work && mkdir -p build/work
echo "== core"
g++ -std=c++20 -O2 -Wall -Wextra -o build/export_test test/export_test.cpp export_core.cpp
./build/export_test "$EXCEL" build/work || status=1
echo "== d2rmm"
node test/run_d2rmm.mjs "$EXCEL" build/work || status=1
exit $status
