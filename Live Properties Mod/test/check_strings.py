#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Nothing may be written into D2RCore's string memory: its tables are rebuilt (Live Strings, another
plugin's strings, a language change) and a write through an old pointer corrupts the heap. Fails when
a pointer from GetNamespacedStringById (g_link.stringById) is cast to non-const, stored in a
non-const char*, kept in a global, or used as the target of memcpy / strcpy / memset / an index store."""
import re, sys

source = open(sys.argv[1], encoding='utf-8').read()
code = re.sub(r'//[^\n]*', '', source)
failed = 0
for n, line in enumerate(code.split('\n'), 1):
    if 'stringById(' not in line:
        continue
    if 'const_cast' in line:
        print('FAIL  line %d: const_cast of a string pointer: %s' % (n, line.strip())); failed += 1
    m = re.search(r'(\b(?:const\s+)?char\s*\*\s*(?:const\s+)?)(\w+)\s*=\s*[^;]*stringById\(', line)
    if m:
        if not m.group(1).strip().startswith('const'):
            print('FAIL  line %d: string pointer kept as non-const char*: %s' % (n, line.strip())); failed += 1
        var = m.group(2)
        lines = code.split('\n')
        end = n
        while end < len(lines) and not lines[end].startswith('}'):  # to the end of this function
            end += 1
        for k in range(n, end + 1):
            other = lines[k - 1]
            if re.search(r'\b(memcpy|memmove|strcpy|strncpy|memset|strcat)\s*\(\s*' + re.escape(var) + r'\b', other) or re.search(r'\b' + re.escape(var) + r'\s*\[[^\]]*\]\s*=[^=]', other):
                print('FAIL  line %d: written through %s (from stringById): %s' % (k, var, other.strip())); failed += 1
    elif re.search(r'\b\w+\s*=\s*[^;]*stringById\(', line) and not re.search(r'\bauto\b', line):
        print('FAIL  line %d: a string pointer kept somewhere: %s' % (n, line.strip())); failed += 1
if re.search(r'g_nameStrings', code):
    print('FAIL  g_nameStrings (pointers into the game\'s strings) is still there'); failed += 1
print('ok    no write into D2RCore\'s string memory' if failed == 0 else '%d problems' % failed)
sys.exit(1 if failed else 0)
