#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
"""Writes legend.txt: what the `prop` command accepts and every stat of the tables the game reads.
usage: make_legend.py [itemstatcost.txt] [strings folder]   (defaults: Exalted Items' copy, the mod's strings)
Read only. The plugin's rules are repeated here by hand (prop_core.h, plugin.cpp)."""
import csv, glob, json, os, re, sys

GAME = os.path.expanduser('~/FI/SteamLibrary/steamapps/common/Diablo II Resurrected')
table = sys.argv[1] if len(sys.argv) > 1 else GAME + '/d2rloader/exalted-items/D2RMM/data/global/excel/itemstatcost.txt'
strings_dir = sys.argv[2] if len(sys.argv) > 2 else GAME + '/mods/D2RMM/D2RMM.mpq/data/local/lng/strings'
SLOTS = range(479, 487)

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
    if i in SLOTS: return 'LIVE', 'a live property\'s own stat (property %d): cannot be A or B' % (487 - i)
    if name.startswith('xlt_unused_'): return 'EMPTY', 'unused filler row'
    if name.startswith('xlt_'): return 'OTHER', 'Exalted Items\' own: leave alone'
    notes = []
    if r['Encode'] not in ('', '0') or r['Save Param Bits'] not in ('', '0'):
        notes.append('carries a parameter (a skill, class, level...): not tried as A or B')
    if r['maxstat']: notes.append('a CURRENT amount (its limit is %s): use the limit instead' % r['maxstat'])
    if (r['op'] or '0') not in ('', '0'): notes.append('is itself an op stat (op %s of %s to %s)' % (r['op'], r['op base'] or '-', r['op stat1'] or '-'))
    if r['damagerelated'] == '1': notes.append('damage-related (the game treats these per weapon): not tried')
    return 'ok', '; '.join(notes)

out = []
w = out.append
w('LIVE PROPERTIES - what the plugin accepts (made by make_legend.py from the tables your game reads)')
w('')
w('1. THE COMMANDS (console, Ctrl + `)')
w('')
w('  prop                               list the 8 properties')
w('  prop set <1-8> <A> <B>             property becomes "N% of A is added to B"')
w('  prop text <1-8> <words>            the item\'s line after "+N% " (up to 90 letters; no tab, quote or backslash)')
w('  prop off <1-8>                     the property does nothing; items keep the stat')
w('  prop give <1-8> <N> [item code]    a test item with the property at N% (N = 1-500; code = 3 or 4 letters of')
w('                                     weapons/armor/misc.txt, default cm1 = small charm; made as a magic item)')
w('')
w('  A and B are stat NAMES exactly as in the Stat column below (lower case matters) or stat NUMBERS.')
w('  An unknown name is refused. A and B must differ; neither can be a live property\'s own stat (479-486).')
w('')
w('2. WHAT A PROPERTY DOES')
w('')
w('  On whoever wears or carries the item:   B += N x A x 41 / 4096      (41 / 4096 = 1.0009%)')
w('  A is the unit\'s whole total of that stat (base + all items + skills), as the game keeps it.')
w('  N is whole numbers, 1 to 500 per item (the save holds -4096..4095). N of several items adds up.')
w('  The game divides by powers of two only, so the result is N x 1.0009%, cut down to a whole number three times')
w('  (up to 3 raw points too low).')
w('')
w('3. UNITS - the part that surprises')
w('')
w('  The game keeps some stats 256 times larger than the number you see (column "kept as": x256).')
w('  The property adds RAW numbers, so:')
w('    A x256 -> B x256   (maxhp -> maxmana)        what you expect: 10% of 1000 life = +100 mana')
w('    A plain -> B plain (strength -> dexterity)   what you expect')
w('    A x256 -> B plain  (maxhp -> strength)       256 times too MUCH: 10% of 1000 life = +25,600 strength')
w('    A plain -> B x256  (strength -> maxhp)       256 times too LITTLE: 10% of 200 strength = +0.08 life')
w('  Regeneration is its own case: hpregen is a plain number that MEANS a small fraction per frame')
w('  (hpregen 1 = 1/256 life per frame = about 0.1 life per second), so "maxhp -> hpregen" is the pair RegenCharm uses:')
w('  1% of max life as hpregen heals about 0.1% of max life per second.')
w('  The plugin does not convert units yet.')
w('')
w('4. LIMITS')
w('')
w('  8 properties (stats 486 down to 479).')
w('  A stat takes 16 additions at most; a live property uses 3 of them. Column "room" = how many more live')
w('  properties can add to that stat.')
w('  A stat can have 64 stats depending on it (as A); column "dep" = how many it has now.')
w('  N x A (raw) should stay below 2,147,483,647: with A x256, keep N x A-as-shown below about 8 million.')
w('  (RegenCharm met a wrap-around there; not re-checked for this plugin.)')
w('')
w('5. EVERY STAT (%d rows; "shown as" = the game\'s own text for it, where it has one)' % len(rows))
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
    room = '-' if k != 'ok' else str((16 - places[i]) // 3)
    w('%-4d %-34s %-7s %-4s %-3s %-44s %s' % (i, r['Stat'], unit, room, dependents[i] if k == 'ok' else '-', shown[:44], notes))
w('')
w('%d unused filler rows (xlt_unused_...) are left out.' % empty)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'legend.txt'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
print('legend.txt: %d lines, %d stats listed' % (len(out), len(rows) - empty))
