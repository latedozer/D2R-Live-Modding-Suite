#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Writes legend.txt: the printed reference of the Legend's words and every stat of the tables the game reads.
usage: make_legend.py [itemstatcost.txt] [strings folder]   (defaults: Exalted Items' copy, the mod's strings)
Read only. The plugin's rules are repeated here by hand (prop_core.h, plugin.cpp)."""
import csv, glob, json, os, re, sys

GAME = os.path.expanduser('~/FI/SteamLibrary/steamapps/common/Diablo II Resurrected')
table = sys.argv[1] if len(sys.argv) > 1 else GAME + '/d2rloader/exalted-items/D2RMM/data/global/excel/itemstatcost.txt'
strings_dir = sys.argv[2] if len(sys.argv) > 2 else GAME + '/mods/D2RMM/D2RMM.mpq/data/local/lng/strings'

rows = list(csv.DictReader(open(table, encoding='utf-8-sig', newline=''), delimiter='\t', quoting=csv.QUOTE_NONE))
text = {}
for path in glob.glob(strings_dir + '/*.json'):
    for entry in json.load(open(path, encoding='utf-8-sig')):
        text[entry.get('Key')] = re.sub('\u00ffc.', '', entry.get('enUS') or '').replace('\n', ' / ')
number = {r['Stat']: i for i, r in enumerate(rows)}
places = [0] * len(rows)      # additions a stat already takes (16 at most)
dependents = [0] * len(rows)  # stats that depend on it (64 at most)
for i, r in enumerate(rows):
    if (r['op'] or '0') not in ('', '0'):
        if r['op base'] in number: dependents[number[r['op base']]] += 1
        for column in ('op stat1', 'op stat2', 'op stat3'):
            if r[column] in number: places[number[r[column]]] += 1
            else: break

def kind(i, r):
    name = r['Stat']
    if name.startswith('xlt_unused_'): return 'EMPTY', 'unused filler row of the older Exalted Items (Stats > New takes the highest of these)'
    if name.startswith('xlt_'): return 'OTHER', 'Exalted Items\' own: leave alone'
    notes = []
    if r['Encode'] not in ('', '0') or r['Save Param Bits'] not in ('', '0'):
        notes.append('carries a parameter (a skill, class, level...)')
    if r['maxstat']: notes.append('a CURRENT amount (its limit is %s)' % r['maxstat'])
    if (r['op'] or '0') not in ('', '0'): notes.append('is itself an op stat (op %s of %s to %s)' % (r['op'], r['op base'] or '-', r['op stat1'] or '-'))
    if r['damagerelated'] == '1': notes.append('damage-related (the game treats these per weapon)')
    return 'ok', '; '.join(notes)

out = []
w = out.append
w('LIVE PROPERTIES 0.8.0 - the printed reference (made by make_legend.py from the tables your game reads)')
w('In game the same words are in the Mod Book\'s Legend (right side): sections Properties, Stats, Affixes.')
w('')
w('1. THE TABS (Mod Book) AND THE CONSOLE (Ctrl + `)')
w('')
w('  Properties  every property row; its card: 7 lines of func / stat / set / val      prop book')
w('  Stats       every stat; its card: 16 cells; New makes a stat of an empty row       prop stats')
w('  Funcs       the property funcs this game has a handler for                       prop funcs')
w('  Affixes     every magic prefix / suffix; New makes one of an empty row           prop affixes')
w('  Buttons stand on the action row under the tabs: Back, Save (N), Discard, Restore; on an affix')
w('  card also Cube only / Rare drops / Normal drops. Typed cells wait (green, *) until Save.')
w('  Kept in d2rloader/live-suite/live-properties/<mod>/: property-edits.txt, stat-edits.txt, affix-edits.txt.')
w('')
w('2. HOW IT WORKS')
w('')
w('  A property is a code (res-all, dmg%) with up to 7 lines; items, affixes, recipes and runewords name')
w('  it with a min, a max and a param. Each line: func = what it does, stat = the stat, set 1 = the value')
w('  replaces the stat\'s (0 = added), val = a number some funcs use (func 21: the param).')
w('  A saved property change counts for items made from then on: items keep the stats they got.')
w('  Funcs 3, 4, 8, 9, 10, 24 take the roll of the line before them (res-all: one roll, four resistances).')
w('')
w('  Units: the game keeps some stats 256 times larger than shown (column "kept as": x256, val shift 8).')
w('  An op works on the kept numbers: an op from a x256 stat to a plain one gives 256 times too much,')
w('  from plain to x256 256 times too little. op 2: op stat += value x op base >> op param')
w('  (the data guide writes <<; the game divides, as RegenCharm found).')
w('')
w('3. LIMITS')
w('')
w('  A stat takes 16 op additions (column "room" = free places of the 16; an edit needing a 17th is refused).')
w('  A stat can be the op base of 64 stats (column "dep" = how many it has now).')
w('  Saved items hold stat numbers 0-510 (9 bits). An op multiplies two kept numbers: keep the result')
w('  below 2,147,483,647 (with x256 stats keep the shown numbers below about 8 million).')
w('  The save cells (signed, send bits, save bits, save add, val shift) are refused while an item holds')
w('  the stat. A character holding an item with a made stat cannot be loaded without this plugin.')
w('')
w('4. EVERY STAT (%d rows; "shown as" = the game\'s own text for it, where it has one)' % len(rows))
w('')
w('%-4s %-34s %-7s %-4s %-3s %-44s %s' % ('no.', 'name', 'kept as', 'room', 'dep', 'shown as', 'notes'))
empty = 0
for i, r in enumerate(rows):
    k, notes = kind(i, r)
    if k == 'EMPTY':
        empty += 1
        continue
    shown = text.get(r['descstrpos'], '') if r['descstrpos'] else ''
    unit = 'x256' if r['ValShift'] == '8' else ('x2^' + r['ValShift'] if r['ValShift'] not in ('', '0') else 'plain')
    room = '-' if k != 'ok' else str(16 - places[i])
    w('%-4d %-34s %-7s %-4s %-3s %-44s %s' % (i, r['Stat'], unit, room, dependents[i] if k == 'ok' else '-', shown[:44], notes))
w('')
w('%d unused filler rows (xlt_unused_...) are left out.' % empty)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'legend.txt'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
print('legend.txt: %d lines, %d stats listed' % (len(out), len(rows) - empty))
