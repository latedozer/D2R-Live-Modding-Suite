#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Builds d2rl-live-tables.dll with mingw-w64 and the native test (build/grow_test).
# SDK=/path/to/PluginSDK/include ./build.sh
set -e
cd "$(dirname "$0")"
SDK="${SDK:-../PluginSDK-master/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p build
x86_64-w64-mingw32-windres -I "$SDK" live-tables.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti -Wno-unknown-pragmas -I "$SDK" \
  -shared -s -static -static-libgcc -static-libstdc++ \
  -o build/d2rl-live-tables.dll plugin.cpp table_model.cpp name_list.cpp build/resources.o -lbcrypt
g++ -std=c++20 -O2 -Wall -Wextra -o build/grow_test test/grow_test.cpp table_model.cpp
g++ -std=c++20 -O2 -Wall -Wextra -o build/names_test test/names_test.cpp table_model.cpp
g++ -std=c++20 -O2 -Wall -Wextra -o build/name_list_test test/name_list_test.cpp name_list.cpp table_model.cpp
ls -la build/d2rl-live-tables.dll
