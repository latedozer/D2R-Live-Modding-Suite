#!/usr/bin/env bash
# Sample the running D2RLoader.exe (D2R) for N seconds (default 20), user-space only.
# Output: tools/perf/<timestamp>/{perf.data,threads.txt,dsos.txt,symbols.txt,maps.txt}
set -euo pipefail
DUR="${1:-20}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PID="$(pgrep -x D2RLoader.exe | head -1 || true)"
[[ -n "$PID" ]] || { echo "D2RLoader.exe not running"; exit 1; }
OUT="$HERE/perf/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
echo "pid=$PID dur=${DUR}s out=$OUT" | tee "$OUT/info.txt"
cp "/proc/$PID/maps" "$OUT/maps.txt"
ls /proc/$PID/task | while read -r t; do echo "$t $(cat /proc/$PID/task/$t/comm)"; done > "$OUT/thread_names.txt"
perf record -e cycles:u -F 999 -p "$PID" -o "$OUT/perf.data" -- sleep "$DUR" 2> "$OUT/record.log"
perf report -i "$OUT/perf.data" --no-children --sort comm,tid --stdio 2>/dev/null | head -60 > "$OUT/threads.txt"
perf report -i "$OUT/perf.data" --no-children --sort dso --stdio 2>/dev/null | head -60 > "$OUT/dsos.txt"
perf report -i "$OUT/perf.data" --no-children --sort tid,dso,sym --stdio 2>/dev/null | head -300 > "$OUT/symbols.txt"
echo done
