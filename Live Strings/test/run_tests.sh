#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# 1. the core: names, revisions, refusals, the string files, the kept file read back at load
# 2. the files it wrote are strict JSON with Key + the 13 languages and no id (python's reader)
# 3. real texts: the mod's own item-modifiers.json (a copy in build/) through the writer and back
# 4. D2RCore.dll: the exports used, the build stamp and the pool bytes poolUse() reads; the DLL's exports
# Only copies in build/ are read; the mod and game folders are only copied from.
set -e
cd "$(dirname "$0")/.."
./build.sh > /dev/null
status=0
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
rm -rf build/files; mkdir -p build/files
echo "== core"
./build/core_test build/files || status=1
echo "== JSON as D2RCore reads it"
python3 -I - build/files <<'PY' || status=1
import json, sys, os
langs = ["enUS","zhTW","deDE","esES","frFR","itIT","koKR","plPL","esMX","jaJP","ptBR","ruRU","zhCN"]
bad = 0; n = 0
for f in sorted(os.listdir(sys.argv[1])):
    d = json.load(open(os.path.join(sys.argv[1], f), encoding="utf-8"))
    for r in d:
        n += 1
        if sorted(r.keys()) != sorted(["Key"] + langs) or not all(isinstance(r[k], str) for k in r): bad += 1; print("FAIL ", f, r.get("Key"))
print(("FAIL" if bad else "ok  ") + "  %d records in %d files: Key + 13 languages, strings only, no id" % (n, len(os.listdir(sys.argv[1]))))
sys.exit(1 if bad else 0)
PY
echo "== real texts (copy of the mod's item-modifiers.json)"
MODSTR="$GAME/mods/D2RMM/D2RMM.mpq/data/local/lng/strings/item-modifiers.json"
if [ -f "$MODSTR" ]; then
  cp "$MODSTR" build/item-modifiers.json
  python3 -I - build/item-modifiers.json build/echo-in.json <<'PY' || status=1
import json, sys, re
langs = ["enUS","zhTW","deDE","esES","frFR","itIT","koKR","plPL","esMX","jaJP","ptBR","ruRU","zhCN"]
d = json.load(open(sys.argv[1], encoding="utf-8-sig"))
have = [l for l in langs if all(l in r for r in d)]
print(("ok  " if have == langs else "FAIL") + "  the mod's file has the 13 languages Live Strings writes: " + " ".join(have))
out = [dict({"Key": "m%d" % i}, **{l: r[l] for l in langs}) for i, r in enumerate(d)]
json.dump(out, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False)
sys.exit(0 if have == langs else 1)
PY
  ./build/core_test echo build/echo-in.json build/echo-out.json || status=1
  python3 -I - build/echo-in.json build/echo-out.json <<'PY' || status=1
import json, sys
a = json.load(open(sys.argv[1], encoding="utf-8")); b = json.load(open(sys.argv[2], encoding="utf-8"))
bad = sum(1 for x, y in zip(a, b) if {k: v for k, v in x.items() if k != "Key"} != {k: v for k, v in y.items() if k != "Key"} or y["Key"] != x["Key"] + "_r1")
bad += abs(len(a) - len(b))
print(("FAIL" if bad else "ok  ") + "  %d real records: every text the same after the writer (%d differ)" % (len(a), bad))
sys.exit(1 if bad else 0)
PY
else
  echo "skip: no $MODSTR"
fi
echo "== D2RCore.dll and the built DLL"
python3 -I - "$GAME/D2RCore.dll" plugin.cpp build/d2rl-live-strings.dll <<'PY' || status=1
import struct, sys, re
core = open(sys.argv[1], "rb").read(); src = open(sys.argv[2]).read(); dll = open(sys.argv[3], "rb").read()
bad = 0; n = 0
def check(ok, what):
    global bad, n
    n += 1
    if not ok: bad += 1; print("FAIL ", what)
pe = struct.unpack_from("<I", core, 0x3c)[0]
stamp = struct.unpack_from("<I", core, pe + 8)[0]
want = int(re.search(r"kKnownCoreStamp\s*=\s*(0x[0-9a-f]+)", src).group(1), 16)
check(stamp == want, "D2RCore stamp %08x, plugin %08x" % (stamp, want))
nsec = struct.unpack_from("<H", core, pe + 6)[0]; so = struct.unpack_from("<H", core, pe + 20)[0]; opt = pe + 24
secs = [struct.unpack_from("<8sIIII", core, opt + so + i * 40) for i in range(nsec)]
def off(r):
    for _, vs, va, rs, ro in secs:
        if va <= r < va + max(vs, rs): return r - va + ro
rva = int(re.search(r"kPoolPrintRva\s*=\s*(0x[0-9a-f]+)", src).group(1), 16)
pr = bytes(int(x, 16) for x in re.search(r"kPoolPrint\[13\]\s*=\s*\{([^}]*)\}", src).group(1).replace(" ", "").split(","))
check(core[off(rva):off(rva) + len(pr)] == pr, "pool bytes at D2RCore 0x%x" % rva)
for name in (b"GetNamespacedStringIdOrDefault\0",):
    check(name in core, "D2RCore export " + name.decode())
for name in (b"LiveStringsGetApi\0", b"D2RLoaderGetPluginInfo\0", b"D2RLoaderLoadPlugin\0"):
    check(name in dll, "DLL export " + name.decode())
print(("FAIL" if bad else "ok  ") + "  %d of %d D2RCore / DLL checks" % (n - bad, n))
sys.exit(1 if bad else 0)
PY
echo "== headers"
for f in *.cpp *.h *.sh *.toml *.rc test/*; do grep -q "SPDX-License-Identifier: GPL-3.0-only" "$f" && grep -q "Copyright (C) 2026 Latedozer" "$f" || { echo "FAIL  no GPL header: $f"; status=1; }; done
[ $status = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $status
