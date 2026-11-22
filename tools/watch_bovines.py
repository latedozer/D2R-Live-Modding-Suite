#!/usr/bin/env python3
"""Read-only watcher for Legacy of Terror's Hell Bovine Minions (plugin 1.0.3) in the running game.
Needs tools/mem_helper.py running (pkexec). Logs each cow's life, mode, place and distance from the
player over its whole life, and how it ended. usage: watch_bovines.py <log> [minutes]"""
import struct, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rd
B = rd.B
LOG = sys.argv[1]
MINUTES = float(sys.argv[2]) if len(sys.argv) > 2 else 27
PRICE_HOOK, COUNT, MINIONS, RAISED, EARLY, GAME = 0x1560, 0x3D140, 0x3D160, 0x3D514, 0x3D50C, 0x3D538  # 1.0.3 build 87e278ac

def log(*a):
    with open(LOG, 'a') as f:
        f.write(time.strftime('%H:%M:%S ') + ' '.join(str(x) for x in a) + '\n')
def many(reqs):
    return rd.read(reqs)
def one(a, n):
    return many([[a, n]]).get(a)
def q(b, o=0):
    return struct.unpack_from('<Q', b, o)[0]
def stats(lst):
    head = one(lst, 0xC0) if lst > 0x10000 else None
    if not head:
        return {}
    p, c = q(head, 0xA8), struct.unpack_from('<I', head, 0xB0)[0]
    d = {}
    if p > 0x10000 and 0 < c < 600:
        a = one(p, c * 16)
        for i in range(c if a else 0):
            layer, _, stat, value = struct.unpack_from('<HHIi', a, i * 16)
            d[(stat, layer)] = value
    return d
def find(game, table_index, uid):
    u = q(one(game + (table_index + (uid & 0x7F)) * 8, 8) or bytes(8))
    for _ in range(64):
        if not u:
            return None
        h = one(u, 0x160)
        if not h:
            return None
        if struct.unpack_from('<I', h, 8)[0] == uid:
            return h
        u = q(h, 0x158)
    return None
def place(h):
    p = one(q(h, 0x38), 8) if q(h, 0x38) > 0x10000 else None
    return (struct.unpack_from('<H', p, 2)[0], struct.unpack_from('<H', p, 6)[0]) if p else (0, 0)
def players(game):
    tab = one(game + 0x446 * 8, 128 * 8)
    out = []
    for b in range(128):
        u = q(tab, b * 8)
        while u:
            h = one(u, 0x160)
            if not h:
                break
            out.append(h)
            u = q(h, 0x158)
    return out

def main():
    cave = one(B + 0x3ECD80, 14)
    dll = q(cave, 6) - PRICE_HOOK
    log(f'plugin at {dll:#x}')
    known, last_counts, last_player = {}, None, None
    end = time.time() + MINUTES * 60
    while time.time() < end:
        blob = one(dll + COUNT, 0x400)
        if blob is None:
            log('game gone'); return
        count = struct.unpack_from('<i', blob, 0)[0]
        game = q(blob, GAME - COUNT)
        counts = (struct.unpack_from('<i', blob, RAISED - COUNT)[0], struct.unpack_from('<i', blob, EARLY - COUNT)[0])
        if counts != last_counts:
            log(f'raised {counts[0]}, slain early {counts[1]}'); last_counts = counts
        if not game or (count <= 0 and not known):
            time.sleep(0.15); continue
        frame = struct.unpack('<I', one(game + 0x170, 4))[0]
        pl = players(game)
        ppos = place(pl[0]) if pl else (0, 0)
        pmode = struct.unpack_from('<I', pl[0], 0xC)[0] if pl else -1
        jump = last_player is not None and (abs(ppos[0] - last_player[0]) + abs(ppos[1] - last_player[1])) > 12
        if jump:
            log(f'   player moved {last_player} -> {ppos} between two looks (a warp or a fast run), mode {pmode}')
        last_player = ppos
        live = set()
        for i in range(max(0, min(count, 64))):
            uid, until = struct.unpack_from('<II', blob, (MINIONS - COUNT) + i * 8)
            live.add(uid)
            h = find(game, 0x4C6, uid)
            k = known.get(uid)
            if h is None:
                if k and not k.get('missing'):
                    k['missing'] = True
                    log(f'   cow {uid}: UNIT GONE from the game at frame {frame} (+{(frame - k["first"]) / 25:.1f}s); last life {k["hp"]:.0f}/{k["mx"]:.0f} mode {k["mode"]} at {k["pos"]}, {k["dist"]:.0f} steps from the player; player now {ppos}')
                continue
            mode = struct.unpack_from('<I', h, 0xC)[0]
            s = stats(q(h, 0x88))
            hp, mx = s.get((6, 0), 0) / 256, s.get((7, 0), 0) / 256
            pos = place(h)
            dist = ((pos[0] - ppos[0]) ** 2 + (pos[1] - ppos[1]) ** 2) ** 0.5
            if k is None:
                k = known[uid] = {'first': frame, 'until': until, 'hp': hp, 'n': 0}
                log(f'COW {uid} at frame {frame}, {(until - frame) / 25:.1f}s of its time left: level {s.get((12, 0))}, life {hp:.0f}/{mx:.0f}, mode {mode}, at {pos}, {dist:.0f} steps from the player {ppos}')
                log(f'   stats: {sorted((st, l, v) for (st, l), v in s.items() if st not in (6, 7))}')
            k['n'] += 1
            if abs(hp - k['hp']) > 1 or mode != k.get('mode') or k['n'] % 12 == 0 or jump:
                log(f'   cow {uid} +{(frame - k["first"]) / 25:.1f}s mode {mode} life {hp:.0f}/{mx:.0f} at {pos} dist {dist:.0f}')
            k.update(hp=hp, mx=mx, mode=mode, pos=pos, dist=dist, frame=frame)
        for uid in list(known):
            if uid not in live:
                k = known.pop(uid)
                left = (k['until'] - k.get('frame', frame)) / 25
                h = find(game, 0x4C6, uid)
                now = struct.unpack_from('<I', h, 0xC)[0] if h else None
                log(f'END cow {uid}: dropped by the plugin {left:.1f}s before its time; last seen +{(k.get("frame", frame) - k["first"]) / 25:.1f}s life {k.get("hp", 0):.0f}/{k.get("mx", 0):.0f} mode {k.get("mode")} dist {k.get("dist", 0):.0f}; unit now {"missing" if h is None else "mode " + str(now)}; player {ppos}')
    log('watch over')

if __name__ == '__main__':
    try:
        main()
    except BaseException as e:
        log('stopped:', repr(e))
