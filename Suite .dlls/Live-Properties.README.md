# Live Properties

`d2rl-live-properties.dll`, version 0.7.0. A D2RLoader plugin for Diablo II: Resurrected, made for
game build 3.4.95041 and D2RLoader 1.3.1.

> **Warning: a character that holds an item with a stat made by Live Properties cannot be loaded
> without the Live Properties plugin** (or a mod exported by Live Export, which has the stat in its
> tables). Take such items off your characters (and out of the stash) before you remove the plugin
> or switch it off.

## What it does

Live Properties changes the game's item tables **while a game runs**, in the game's memory. No
table reload, no restart. The mod's own files are never written.

- **Properties**: every row of the property table can be looked at and its 7 lines changed
  (func, stat, set, val).
- **Stats**: every stat can be looked at; its row can be changed, and new stats can be made from
  the table's empty rows.
- **Affixes**: every magic prefix and suffix can be looked at and changed, and new ones made from
  the tables' empty rows - for example a charm affix that only a cube recipe gives.

A change counts for items made from then on. Items that exist keep the stats they have.

Version 0.7.0 no longer has the "live properties" of earlier versions (`prop set`, `prop give` ...):
every property is now changed the same way, in the Properties tab.

## What it needs

- **Mod Book** (`d2rl-mod-book.dll`) for the tabs. Without it the plugin has no screen.
- **Save Scan** (`d2rl-save-scan.dll`) for its safeguards (see below). Strongly advised.
- **Exalted Items** (`d2rl-exalted-items.dll`) only for **making new stats**: a new stat takes one of
  the empty rows (`xlt_unused_...`) Exalted Items leaves in its stat table. Everything else works
  without it.

## The tabs (with Mod Book)

On every card: click a cell, type, Enter keeps it (Tab: next cell, Escape: leave it). Kept cells
show green with a `*`. **Save changes (N)** sends them to the game, **Discard** forgets them, **The
game's own row** puts the row back as the game read it. You cannot leave a card with unsaved
changes.

- **Properties** (left, Ctrl+B): every row of the game's property table. A card has 7 lines of
  func / stat / set / val. With a func or stat cell open, a click in the Funcs or Stats tab puts
  that entry in.
- **Stats** (right, Ctrl+L, group "Properties"): every stat. A card shows how the stat is saved, how
  it is shown, and its op (what it does to other stats). **New** makes a stat from an empty row (13
  bits, may be negative, no text, no op); give it a name, a line of text and an op on its card, and
  put it on items through a property line with func 1. **Remove this stat** empties a made stat.
- **Funcs** (right, group "Properties"): every func the game has, read from the game itself.
- **Affixes** (left, Ctrl+B): every prefix and suffix (and automagic row). A card has 35 cells:
  name, spawnable, rare, level, maxlevel, levelreq, frequency, group, the class cells, three mods
  (property code, param, min, max) and the item types it may (itype) or may not (etype) be on.
  **New** fills an empty prefix or suffix row; its number never moves, so recipes and saved items
  keep pointing at it. A name that is not one of the game's own gets a string of the plugin's (16
  at most). Three buttons fill the cells for how the affix may appear:
  - **Cube only**: level 99, frequency 1. Give the recipe `lvl` 99. Drops seldom reach level 99.
  - **Rare drops**: frequency 1. A recipe can force it, and it may also drop, seldom.
  - **Normal drops**: frequency 4, like most of the game's affixes.

  In a recipe, force it with `pre=N` or `suf=N` (the card shows N), e.g. `cm1,mag,pre=9`. The game
  takes a forced affix only if spawnable is 1, frequency is above 0, its level is reached and the
  item's type is listed. A charm always gets at least one affix, so a charm with one forced
  prefix carries that prefix and nothing random.

## Console commands

The console opens with Ctrl + `.

| Command | Does |
|---|---|
| `prop book` | opens the Properties tab |
| `prop stats` / `prop funcs` / `prop affixes` | opens that tab |

## Safeguards

- A change to how a stat is **saved** (signed, bits, save add, val shift) is refused while you wear
  or carry an item with that stat.
- With **Save Scan** installed, every saved character and the shared stash are read first. The
  change is refused if any saved item holds the stat, or if a save file could not be read.
- Without Save Scan, saved characters and the stash are **not** checked; the card says so.
- A stat the game names itself cannot be renamed.

## Installing

1. Close the game.
2. Copy `d2rl-live-properties.dll` into `Diablo II Resurrected/d2rloader/plugins/`. Also copy
   `d2rl-mod-book.dll` and `d2rl-save-scan.dll` there.
3. Start the game through D2RLoader, enter a game, open the Mod Book (Ctrl+B).

## Files

Everything is in `Diablo II Resurrected/d2rloader/live-suite/live-properties/<mod>/` (`<mod>` = the
mod you play, `D2RMM` for D2RMM; an older `d2rloader/live-properties/<mod>/` folder is moved there
at the first start). Each file is put back every time the game reads its tables.

| File | What it is |
|---|---|
| `property-edits.txt` | the property rows you changed. Delete a line to get the game's own row back. |
| `stat-edits.txt` | the stats you changed or made. Delete a line to get the game's own row back. |
| `affix-edits.txt` | the affixes you changed or made. Delete a line to get the game's own row back. |
| `string-keys.txt` | string keys you typed into stat cells (Live Export uses them) |

Settings: `d2rloader/config/live-properties.toml` (`enabled`). Close the game before changing it.
Log: `d2rloader/logs/live-properties.log`.

## Known limits

- Changing property rows and making stats with **New** have been used in the game. Changing and
  removing stats, the Funcs tab, the Save changes button and the Affixes tab are new and not yet
  tried in the game.
- If a character's totals do not follow a change, take the item off and put it on again.
- Only the character in the game is checked for carried items; other characters and the stash are
  checked only through Save Scan.
- New property rows cannot be made yet; existing ones can be changed.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
