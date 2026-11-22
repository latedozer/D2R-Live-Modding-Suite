#!/bin/bash
# Builds d2rl-live-cube-recipes.dll with mingw-w64 (Arch: pacman -S mingw-w64-gcc).
# Needs the D2RLoader Plugin SDK headers: SDK=/path/to/PluginSDK/include ./build.sh
set -e
cd "$(dirname "$0")"
SDK="${SDK:-../PluginSDK-master/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p build/casclib
# CascLib (third_party/) reads the game's own button picture when the mod has no copy of it.
CASC=third_party/CascLib
CASC_DEFS=(-DCASCLIB_NO_AUTO_LINK_LIBRARY -DCASCLIB_NODEBUG -DUNICODE -D_UNICODE -DCASCLIB_UNICODE)
if [ ! -f build/casclib/libcasc.a ]; then
	FILES=$(sed -n '/set(SRC_FILES/,/^)/p' $CASC/CMakeLists.txt | grep -oE 'src/[^ ]+\.(c|cpp)' | sort -u)
	FILES="$FILES src/zlib/adler32.c src/zlib/crc32.c src/zlib/inffast.c src/zlib/inflate.c src/zlib/inftrees.c src/zlib/zutil.c"
	for f in $FILES; do
		case $f in *.c) CC=(x86_64-w64-mingw32-gcc);; *) CC=(x86_64-w64-mingw32-g++ -std=c++17);; esac
		"${CC[@]}" -O2 -w -c -D__CASCLIB_SELF__ "${CASC_DEFS[@]}" "$CASC/$f" -o "build/casclib/$(echo "$f" | tr / _).o"
	done
	x86_64-w64-mingw32-ar rcs build/casclib/libcasc.a build/casclib/*.o
fi
x86_64-w64-mingw32-windres -I "$SDK" live-cube-recipes.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti -Wno-unknown-pragmas -I "$SDK" -I $CASC/src "${CASC_DEFS[@]}" \
  -shared -s -static -static-libgcc -static-libstdc++ \
  -o build/d2rl-live-cube-recipes.dll plugin.cpp cube_core.cpp build/resources.o build/casclib/libcasc.a -lws2_32
ls -la build/d2rl-live-cube-recipes.dll
