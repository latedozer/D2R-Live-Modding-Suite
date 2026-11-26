#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""What runs from a Live Tables done() callback (and every Guard entry) must not throw: the plugin is
built with -fno-exceptions, so a throw is a fatal error (the 2026-11-25 21:38 crash). Checks:
 1. no member is set to a pointer into a vector's elements (x.y = v.data() / &v[0]): a re-walk after a
    grow moves the elements (affix::Names now holds the vectors themselves). A request struct the
    callee copies during the call is marked '// copied during the call';
 2. no throwing accessor or conversion on game data: .at( std::sto* stoi( stol( stoul(."""
import re, sys

files = ['plugin.cpp', 'lt_core.cpp', 'affix_core.cpp', 'legend_core.cpp', 'prop_core.cpp', 'card_core.cpp', 'op_core.cpp']
failed = 0
for name in files:
    for n, line in enumerate(open(name, encoding='utf-8'), 1):
        code = line.split('//')[0]
        if 'copied during the call' not in line and re.search(r'\w+(\.|->)\w+\s*=\s*[^;=]*(\.data\(\)|&\w+(\.|->)?\w*\[0\])\s*;', code):
            print('FAIL  %s:%d: a member points into a vector\'s elements: %s' % (name, n, line.strip()))
            failed += 1
        if re.search(r'\.at\(|std::sto[a-z]+\(|\bsto(i|l|ul|ll|ull|f|d)\(', code):
            print('FAIL  %s:%d: a throwing call: %s' % (name, n, line.strip()))
            failed += 1
names = open('affix_core.h', encoding='utf-8').read()
if not re.search(r'const std::vector<std::string>\*\s+props', names):
    print('FAIL  affix_core.h: affix::Names.props is not a pointer to the vector')
    failed += 1
print('ok    no member points into a vector\'s elements; no .at / sto* calls (%d files)' % len(files) if failed == 0 else '%d problems' % failed)
sys.exit(1 if failed else 0)
