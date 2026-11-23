#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Checks plugin.cpp's addresses against a dump of the game image it was written for.
usage: check_prints.py <plugin.cpp> <image dump (file offset == RVA)>"""
import re, struct, sys

source = open(sys.argv[1]).read()
try:
    image = open(sys.argv[2], 'rb').read()
except OSError:
    print('skip  no image dump at', sys.argv[2]); sys.exit(0)
failed = 0
def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what); failed += 0 if ok else 1

constants = {name: int(value, 16) for name, value in re.findall(r'constexpr \w+ (k\w+)\s*=\s*(0x[0-9a-fA-F]+);', source)}
stamp = struct.unpack_from('<I', image, struct.unpack_from('<I', image, 0x3c)[0] + 8)[0]
check(stamp == constants['kKnownStamp'], 'image stamp %08x is the one the plugin knows' % stamp)
prints = re.findall(r'\{ (k\w+|0x[0-9a-f]+), "([^"]+)", (true|false), (\d+), \{ ([^}]+) \} \}', source)
check(len(prints) == 19, '%d fingerprints found in plugin.cpp' % len(prints))
for where, what, entry, size, data in prints:
    rva = constants[where] if where in constants else int(where, 16)
    want = bytes(int(b, 16) for b in data.split(','))
    check(len(want) == int(size) and image[rva:rva + len(want)] == want, '%s at 0x%x' % (what, rva))
sys.exit(1 if failed else 0)
