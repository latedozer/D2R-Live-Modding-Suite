#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Read-only: find player units through the client unit table and dump their regen-related stats.
Run as root (pkexec) while in game. Output: tools/probe_player.log"""
import os, struct, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "probe_player.log")
lines = []
CLIENT_UNIT_TABLE = 0x2A23910  # + type*0x400, 128 buckets, chain at unit+0x158 (build 93847)
WANT = {6: "hitpoints", 7: "maxhp", 74: "hpregen", 371: "item_hpregen_permaxlife",
        372: "permaxlife_display", 374: "permaxlife_total"}


def log(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    lines.append(s)


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
        log("game not running")
        return
    mem = open(f"/proc/{pid}/mem", "rb", buffering=0)

    def rd(a, n):
        if not a or a < 0x10000:
            return None
        try:
            mem.seek(a)
            return mem.read(n)
        except (OSError, ValueError, OverflowError):
            return None

    def q(a):
        b = rd(a, 8)
        return struct.unpack("<Q", b)[0] if b else 0

    def stat_array(ptr, count):
        b = rd(ptr, count * 8)
        if not b:
            return None
        ent = [struct.unpack_from("<Ii", b, i * 8) for i in range(count)]
        keys = [k for k, _ in ent]
        if keys != sorted(keys):
            return None
        return ent

    def dump_list(lst, tag):
        blob = rd(lst, 0x2B00)
        if not blob:
            log(f"  {tag}: unreadable list {lst:#x}")
            return
        for off in range(0, len(blob) - 12, 8):
            ptr = struct.unpack_from("<Q", blob, off)[0]
            cnt = struct.unpack_from("<I", blob, off + 8)[0]
            if not (0 < cnt < 600) or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
                continue
            ent = stat_array(ptr, cnt)
            if not ent:
                continue
            sel = [(k >> 16, k & 0xFFFF, v) for k, v in ent if (k >> 16) in WANT]
            if not sel:
                continue
            desc = "  ".join(f"{WANT[s]}{'' if l == 0 else '/' + str(l)}={v}" + (f"({v / 256:.1f})" if s in (6, 7) else "") for s, l, v in sel)
            log(f"  {tag}+{off:#x} array@{ptr:#x} n={cnt}: {desc}")

    log(f"pid {pid} base {base:#x}")
    seen = 0
    for typ in (0,):
        for bkt in range(128):
            u = q(base + CLIENT_UNIT_TABLE + typ * 0x400 + bkt * 8)
            hops = 0
            while u and hops < 64:
                hdr = rd(u, 0x10)
                if not hdr:
                    break
                utype, uclass, uid = struct.unpack_from("<III", hdr, 0)
                lst = q(u + 0x88)
                log(f"unit {u:#x} type={utype} class={uclass} id={uid} statlist={lst:#x}")
                if lst:
                    dump_list(lst, "statlist")
                seen += 1
                u = q(u + 0x158)
                hops += 1
    log(f"{seen} player units in the client table")


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
