#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Linux/Proton port of d2rAI-toolkit tools/dump_image.py.

Dumps the running game image (the main D2RLoader.exe module, which D2RLoader fills
with the decrypted D2R code at start-up) into a "memory layout" PE: every section's
file offset == its RVA, ImageBase = load address. Read-only. Needs root because the
kernel (yama ptrace_scope=1) blocks reading another process's memory.

Output: ../bin/D2RLoader_<timestamp>_dump.exe   Log: ./dump.log
"""
import os, struct, sys, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.normpath(os.path.join(HERE, "..", "bin"))
LOG = os.path.join(HERE, "dump.log")
log_lines = []


def log(msg):
    print(msg)
    log_lines.append(str(msg))


def find_loader():
    """Return (pid, base) of the process that maps D2RLoader.exe as its main image."""
    best = None
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/maps") as f:
                bases = [int(l.split("-")[0], 16) for l in f if l.rstrip().lower().endswith("/d2rloader.exe")]
        except OSError:
            continue
        if bases:
            best = (int(p), min(bases))
    return best


def main():
    found = find_loader()
    if not found:
        log("D2RLoader.exe is not running. Start the game through D2RLoader first.")
        return 1
    pid, base = found
    try:
        mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
    except PermissionError:
        log("Permission denied: run this with root (use run_dump.sh).")
        return 1

    def read(addr, n):
        try:
            mem.seek(addr)
            return mem.read(n)
        except OSError:
            return None

    head = read(base, 0x1000)
    if not head or head[:2] != b"MZ":
        log(f"No MZ header at {base:#x} in pid {pid}.")
        return 1
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    nsec = struct.unpack_from("<H", head, pe + 6)[0]
    ts = struct.unpack_from("<I", head, pe + 8)[0]
    opt = pe + 24
    soi = struct.unpack_from("<I", head, opt + 56)[0]
    log(f"pid {pid}, base {base:#x}, image {soi:#x} bytes, {nsec} sections, timestamp {ts:#010x}")

    image = bytearray(soi)
    bad = 0
    for off in range(0, soi, 0x1000):
        b = read(base + off, 0x1000)
        if b:
            image[off:off + len(b)] = b
        else:
            bad += 1
    sec = opt + struct.unpack_from("<H", image, pe + 20)[0]
    for i in range(nsec):
        o = sec + 40 * i
        vsize, va = struct.unpack_from("<II", image, o + 8)
        struct.pack_into("<II", image, o + 16, vsize, va)  # raw size/offset = virtual
        name = image[o:o + 8].rstrip(b"\0").decode(errors="replace")
        log(f"  {name:8} rva {va:#010x} size {vsize:#010x}")
    struct.pack_into("<Q", image, opt + 24, base)

    os.makedirs(BIN, exist_ok=True)
    out = os.path.join(BIN, f"D2RLoader_{ts:08x}_dump.exe")
    with open(out, "wb") as f:
        f.write(image)
    uid = int(os.environ.get("PKEXEC_UID") or os.environ.get("SUDO_UID") or os.getuid())
    try:
        os.chown(out, uid, -1)
    except OSError:
        pass
    log(f"wrote {out} ({len(image) / 1e6:.0f} MB, {bad} unreadable pages)")
    return 0


if __name__ == "__main__":
    rc = 1
    try:
        rc = main()
    except Exception:
        log(traceback.format_exc())
    finally:
        with open(LOG, "w") as f:
            f.write("\n".join(log_lines) + "\n")
        try:
            os.chmod(LOG, 0o666)
        except OSError:
            pass
    sys.exit(rc)
