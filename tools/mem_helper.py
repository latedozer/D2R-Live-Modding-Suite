#!/usr/bin/env python3
"""Read-only memory helper for the running D2RLoader.exe (run once as root via pkexec).

Serves read requests for the game process only: Claude writes tools/rpc/req.json
([[addr, size], ...]), this writes tools/rpc/resp.json ({"addr": "hex" | null}).
Never writes game memory. Exits after 30 minutes, when the game exits, or when
tools/rpc/stop exists.
"""
import json, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
RPC = os.path.join(HERE, "rpc")
REQ, RESP, STOP = (os.path.join(RPC, n) for n in ("req.json", "resp.json", "stop"))
uid = int(os.environ.get("PKEXEC_UID", "1000"))
os.makedirs(RPC, exist_ok=True)
os.chown(RPC, uid, -1)


def find_pid():
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/maps") as f:
                if any(l.rstrip().lower().endswith("/d2rloader.exe") for l in f):
                    return int(p)
        except OSError:
            pass
    return None


pid = find_pid()
if pid is None:
    raise SystemExit("game not running")
mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
end = time.time() + 1800
with open(os.path.join(RPC, "ready"), "w") as f:
    f.write(str(pid))
while time.time() < end and os.path.exists(f"/proc/{pid}") and not os.path.exists(STOP):
    if os.path.exists(REQ):
        try:
            with open(REQ) as f:
                reqs = json.load(f)
            out = {}
            for addr, size in reqs[:4096]:
                size = min(int(size), 0x400000)
                try:
                    mem.seek(int(addr))
                    out[str(addr)] = mem.read(size).hex()
                except (OSError, ValueError, OverflowError):
                    out[str(addr)] = None
            tmp = RESP + ".tmp"
            with open(tmp, "w") as f:
                json.dump(out, f)
            os.chown(tmp, uid, -1)
            os.replace(tmp, RESP)
        except Exception as e:
            with open(RESP, "w") as f:
                json.dump({"error": repr(e)}, f)
        os.remove(REQ)
    time.sleep(0.05)
for n in ("ready",):
    try:
        os.remove(os.path.join(RPC, n))
    except OSError:
        pass
