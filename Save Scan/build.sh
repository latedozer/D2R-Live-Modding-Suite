#!/bin/bash
# Builds d2rl-save-scan.dll with mingw-w64. SDK=/path/to/PluginSDK/include ./build.sh
set -e
cd "$(dirname "$0")"
SDK="${SDK:-../PluginSDK-master/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p build
x86_64-w64-mingw32-windres -I "$SDK" save-scan.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti -Wno-unknown-pragmas -I "$SDK" \
  -shared -s -static -static-libgcc -static-libstdc++ \
  -o build/d2rl-save-scan.dll plugin.cpp save_core.cpp build/resources.o
ls -la build/d2rl-save-scan.dll
