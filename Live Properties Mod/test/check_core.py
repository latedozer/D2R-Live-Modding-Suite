#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""The D2RCore routines plugin.cpp patches or calls, against the D2RCore.dll given (read only):
every '// d2rcore 0x<rva>: <bytes>' line in plugin.cpp must match the DLL's bytes at that RVA."""
import re, struct, sys

source = open(sys.argv[1], encoding='utf-8').read()
data = open(sys.argv[2], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
count = struct.unpack_from('<H', data, pe + 6)[0]
optional = struct.unpack_from('<H', data, pe + 20)[0]

def offset(rva):
    for i in range(count):
        at = pe + 24 + optional + 40 * i
        size, va, raw_size, raw = struct.unpack_from('<IIII', data, at + 8)
        if va <= rva < va + size:
            return raw + rva - va
    return None

failed = seen = 0
for rva, want in re.findall(r'// d2rcore 0x([0-9a-f]+): ([0-9a-f ]+)', source):
    rva, want = int(rva, 16), bytes.fromhex(want)
    at = offset(rva)
    got = data[at:at + len(want)] if at is not None else b''
    seen += 1
    if got != want:
        failed += 1
        print('FAIL  0x%x: plugin.cpp expects %s, D2RCore has %s' % (rva, want.hex(' '), got.hex(' ')))
    else:
        print('ok    0x%x: %d bytes as expected' % (rva, len(want)))
sys.exit(1 if failed or seen == 0 else 0)
