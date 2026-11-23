#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Read-only probe for the Replenish Life drain (run as root via pkexec while in game).

1. Checks that stat-op-overflow-fix.json's bytes are really in memory.
2. Scans the game's heap for stat-list entries (key = stat << 16 | layer, then i32 value)
   and prints hpregen (74) next to hitpoints (6) / maxhp (7) / item_hpregen_permaxlife (371).
Writes everything to tools/probe.log. Changes nothing.
"""
import os, re, struct, sys, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "probe.log")
lines = []


def log(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    lines.append(s)


CHECKS = {  # rva: expected patched bytes (prefix)
    0x2F8E9F: bytes.fromhex("E95C6F9B01"),
    0x2F916B: bytes.fromhex("E9106D9B01"),
    0x1CAFE00: bytes.fromhex("0FB68EDF0000004863C0480FAFC3"),
    0x1CAFE80: bytes.fromhex("410FB68FDF0000004863C0480FAFC3"),
}
STATS = {6: "hitpoints", 7: "maxhp", 74: "hpregen", 371: "item_hpregen_permaxlife", 374: "item_hpregen_permaxlife_total"}


def main():
    pid = base = None
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/maps") as f:
                b = [int(l.split("-")[0], 16) for l in f if l.rstrip().lower().endswith("/d2rloader.exe")]
        except OSError:
            continue
        if b:
            pid, base = int(p), min(b)
    if pid is None:
        log("D2RLoader.exe is not running.")
        return
    mem = open(f"/proc/{pid}/mem", "rb", buffering=0)

    def rd(a, n):
        try:
            mem.seek(a)
            return mem.read(n)
        except OSError:
            return None

    log(f"pid {pid} base {base:#x}")
    for rva, exp in CHECKS.items():
        got = rd(base + rva, len(exp))
        log(f"patch @ {rva:#09x}: {'OK' if got == exp else 'MISSING'}  live={got.hex() if got else None}")

    # scan writable anonymous memory for hpregen entries
    key74 = struct.pack("<I", 74 << 16)
    regions = []
    with open(f"/proc/{pid}/maps") as f:
        for l in f:
            parts = l.split()
            if len(parts) < 5 or not parts[1].startswith("rw"):
                continue
            if len(parts) >= 6 and parts[5].startswith("/"):
                continue
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            if hi - lo > 0x40000000:
                continue
            regions.append((lo, hi))
    found = 0
    for lo, hi in regions:
        CH = 0x4000000
        for a in range(lo, hi, CH):
            buf = rd(a, min(CH, hi - a))
            if not buf:
                continue
            for m in re.finditer(re.escape(key74), buf):
                o = m.start()
                if o % 4 or o < 0x200:
                    continue
                # look back for maxhp and hitpoints keys in the same sorted list
                win = buf[max(0, o - 0x600):o + 0x200]
                start = max(0, o - 0x600)
                vals = {}
                for sid in STATS:
                    k = struct.pack("<I", sid << 16)
                    for mm in re.finditer(re.escape(k), win):
                        if (start + mm.start()) % 4 == 0 and (start + mm.start()) % 8 == o % 8:
                            vals[sid] = struct.unpack_from("<i", win, mm.start() + 4)[0]
                            break
                if 7 not in vals or not (256 <= vals[7] <= 0x7FFFFFFF):
                    continue
                vals[74] = struct.unpack_from("<i", buf, o + 4)[0]
                found += 1
                desc = "  ".join(f"{STATS[s]}={v}" + (f" ({v >> 8} pts)" if s in (6, 7) else "") for s, v in sorted(vals.items()))
                log(f"@{a + o:#x}: {desc}")
                if found >= 60:
                    log("... stopping after 60 hits")
                    return
    log(f"{found} stat-list hits with hpregen next to maxhp")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        log(traceback.format_exc())
    finally:
        with open(LOG, "w") as f:
            f.write("\n".join(lines) + "\n")
        try:
            os.chmod(LOG, 0o666)
        except OSError:
            pass
