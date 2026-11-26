#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Suite rule (Mod Book 0.8.0): a label on a 1-slot action() button is at most 12 letters, or it
wraps out of the button; longer labels take 2 slots. Reads every Actions::add(...) call and every
CardActions(...) call (its 3rd argument is the 4th button's label) in plugin.cpp.
A label made of a text and a number (\"Save (\" + count + \")\") counts the number as 2 digits."""
import re, sys

MOST = 12
source = open(sys.argv[1] if len(sys.argv) > 1 else 'plugin.cpp', encoding='utf-8').read()

def args(text, start):
    """The top-level arguments of the call whose '(' is at start."""
    depth, at, parts, cur, quote = 0, start, [], '', False
    while at < len(text):
        c = text[at]
        if quote:
            cur += c
            if c == '\\':
                cur += text[at + 1]; at += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True; cur += c
        elif c in '([{':
            depth += 1
            if depth > 1: cur += c
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                parts.append(cur.strip()); return parts
            cur += c
        elif c == ',' and depth == 1:
            parts.append(cur.strip()); cur = ''
        else:
            cur += c
        at += 1
    return parts

def lengths(expr):
    """The lengths the label can have: each branch of a ?: on its own; + joins texts."""
    branches = re.split(r'\?|(?<!:):(?!:)', expr) if '?' in expr else [expr]
    out = []
    for b in branches:
        texts = re.findall(r'"((?:[^"\\]|\\.)*)"', b)
        if not texts:
            continue
        n = sum(len(t) for t in texts) + (2 * b.count('to_string') if '+' in b else 0)
        out.append((n, b.strip()))
    return out

failed, seen = 0, 0
for m in re.finditer(r'\b(row|ask|\w+)\.add\(|CardActions\(', source):
    call = args(source, m.end() - 1)
    card = m.group(0).startswith('CardActions')
    if card:
        if len(call) < 3 or call[0].startswith('modbook::Page'):
            continue  # the definition
        label, slots = call[2], '1'
    else:
        if len(call) < 2 or call[0].startswith('const char*'):
            continue  # the definition
        label, slots = call[1], call[3] if len(call) > 3 else '1'
    if slots.strip() not in ('1', '0'):
        continue
    for n, text in lengths(label):
        seen += 1
        if n > MOST:
            failed += 1
            line = source.count('\n', 0, m.start()) + 1
            print('FAIL  plugin.cpp:%d: a 1-slot button label of %d letters (most %d): %s' % (line, n, MOST, text))
print(('ok    %d one-slot button labels, all %d letters or fewer' % (seen, MOST)) if failed == 0 else 'labels: %d FAILED' % failed)
sys.exit(1 if failed or seen == 0 else 0)
