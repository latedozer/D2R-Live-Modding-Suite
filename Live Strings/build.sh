#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# Builds d2rl-live-strings.dll with mingw-w64 and the native test (build/core_test).
# SDK=/path/to/PluginSDK/include ./build.sh
set -e
cd "$(dirname "$0")"
SDK="${SDK:-../PluginSDK-master/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p build
x86_64-w64-mingw32-windres -I "$SDK" live-strings.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti -Wno-unknown-pragmas -I "$SDK" \
  -shared -s -static -static-libgcc -static-libstdc++ \
  -o build/d2rl-live-strings.dll plugin.cpp strings_core.cpp build/resources.o -lbcrypt
g++ -std=c++20 -O2 -Wall -Wextra -o build/core_test test/core_test.cpp strings_core.cpp
ls -la build/d2rl-live-strings.dll
