<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Latedozer. -->
# Live Properties - version 0.12.0

`d2rl-live-properties.dll`: a D2RLoader plugin for Diablo II: Resurrected (game 3.4.95041,
D2RLoader 1.3.1). It changes the game's stat, property and magic affix rows **while a game runs**,
in memory. No reload, no restart; the mod's own files are never written.

> **Warning:** a character holding an item with a stat made here can't be loaded without Live
> Properties and Live Tables (or a mod exported by Live Export). Take such items off first.

## What it does

The tabs are in the Mod Book: Properties and Affixes on the left (Ctrl+B), Stats and Funcs on the
right (Ctrl+L). On a card, click a cell, type, then Enter, Tab or a click elsewhere keeps it
(green, `*`). **Save (N)** sends every kept cell; **Discard** forgets them.

- **Properties:** every row's 7 lines (func / stat / set / val). Funcs 1/2/3/8 take their amount
  from the min..max of the recipe, affix or item that names the code; val isn't used.
  - Disable / Enable.
  - **New property** (code, added at once). Rename and Remove for your own; Remove takes only
    the last grown row.
- **Stats:** every row (saved shape, text, op).
  - **New** (`stat<n>`, 13 bits), Disable / Enable (+ "Disable all" for the properties naming
    it), Rename and Remove for your own.
  - Remove shows Save Scan's dry run first; Live Tables then fixes the saves and removes the row.
    The last row can be removed in a game, a middle row only at the main menu.
  - **Adv:** never / always / not 0: whether the Advanced Stats panel shows it (`advdisplay`).
  - **Game view:** how the game holds the row (saved shape and range, op in words, who adds to
    it, tooltip line and its place in the order, advdisplay) and what it does on your character now.
- **Funcs:** the property funcs the game has.
- **Affixes:** every prefix / suffix / automagic row, 35 cells.
  - Presets Cube only / Rare drops / Normal drops; Disable / Enable (spawnable and frequency 0).
  - New in an empty row; Remove of your own, refused while a saved item carries it.
  - In a recipe: `pre=N` / `suf=N`.
- Rows are added, removed, renamed and (for made rows) changed by **Live Tables** (setCells: it keeps them). Without it, the tabs only change the rows
  that are there.
- The lists start with "N of M used", and with a WARNING when a made stat's number moved at a start.

Saved affix ids: an item saves an affix as its row's place in its kind + 1. That's proven for rows
after the files' Expansion line. The classic block before it (where the empty rows are) is held
back until `prop affixtest` has confirmed it once:
1. type it in a game, on a throwaway character;
2. save and quit;
3. start again, and the log says whether it's confirmed.

## Needs

- Mod Book (the tabs).
- Live Tables 0.3.0+ (new, removed and renamed rows).
- Save Scan 0.3.3+ (save checks, dry run, affix holders).
- Exalted Items is not needed.

## Build, test, install

- `./build.sh`: mingw-w64; the SDK comes from `../PluginSDK-master`.
- `./test/run_tests.sh`: prop_core against the game's linker port; affixes against the vanilla
  tables; legend; card typing; op formulas; lt_core against a fake Live Tables. Also:
  - the saved affix ids, on copies of the saves, with Save Scan's reader and the D2RMM mod
    PerfectDrops run on copies;
  - check_noexcept, button labels, and the 20 address prints against the dump.
- `./install.sh`, only when the user says "install"; it refuses while the game runs. It keeps the
  replaced DLL in `Suite .dlls/previous/` and adds time / version / sha256 to
  `d2rloader/live-suite/installed.txt`.
- The log's first line names the version and the sha256 of the DLL that ran.

## API (`live_props_api.h`, version 2)

`liveprops::Find()` then:
- `findProperty(code, dataVersion)` gives the property row or -1;
- `listMade(table, dataVersion, out, room)` gives the stats / properties made here that the game
  has now, in row order.

Any thread. `LivePropsGetApi(v)` answers any v >= 1 with the newest Api (check `version`). The DLL also exports
`LivePropsUsesLiveTables`.

## Files (`d2rloader/live-suite/live-properties/<mod>/`)

| File | What |
|---|---|
| `property-edits.txt` | changed property rows (by code) |
| `property-off.txt` | disabled properties |
| `made-properties.txt` | property codes made here |
| `stat-edits.txt` | changed game / mod stats (number, 16 cells, made name, row name) |
| `made-stats.txt` | the stats made here (Live Tables keeps their cells in grown-rows.txt) |
| `stat-adv.txt` | game / mod stats whose advdisplay was changed (name, 0 / 1 / 2) |
| `string-keys.txt` | string keys typed into stat cells (for Live Export) |
| `affix-edits.txt` / `affix-off.txt` | changed or made affixes / disabled affixes |
| `affix-ids.txt` | whether the classic block's saved affix ids are confirmed |
| `*.before-<what>-<date>.txt` | dated copies made before a renumber, rename or remove |

Settings: `d2rloader/config/live-properties.toml` (`enabled`). Log:
`d2rloader/logs/live-properties.log`. Console: `prop book | stats | funcs | affixes | affixtest`.

## Known limits

- 0.12.0 hasn't run in a game (0.11.6 is installed).
- Totals that don't follow a change come back when the item is taken off and put on.
- Only the character in the game is checked for carried items; the rest goes through Save Scan.
- A made stat's text can only use existing string keys (new text needs Live Strings).
- An affix name in your own words shows as the plugin's fixed text ("live affix name N") in the game until Live Strings gives it words (1.3b); the card shows your words.

## Changes

- 0.12.0: Advanced Stats cell (advdisplay) and the stat card's Game view (op formulas tested).
- 0.11.6: the first-start handover leaves the save-shape cells to Live Tables; GetApi answers any version >= 1; dated backup of made-stats.txt before a rename.
- 0.11.5: no writes into the game's string memory (own-words affix names keep a fixed text in game); made rows' cells through Live Tables' setCells; made stats' old lines handed over once (made-stats.txt).
- 0.11.4: `prop affixtest`, affix New/Remove guarded by saved ids, Live Tables' alarm on screen.
- 0.11.3: GetApi for any older version, sha256 in the log, install.sh keeps the old DLL.
- 0.11.2: Rename (stats, properties), property Remove.
- 0.11.1: affix Disable/Enable; the func 1/2/3/8 note on property cards.
- 0.11.0: card typing keeps every cell; stat Disable/Enable/Remove.
- 0.10.x: rows through Live Tables (New at once); 477/478 handed over; crash fix in the Affixes list.
- 0.9.x: table-text rows (relative paths, no spare rows); API v2 listMade.
- 0.8.0: action row, Legend sections. 0.7.0: the 8 "live properties" removed; whole tables editable.
- 0.6.0: Affixes tab. 0.5.0: Save / Discard. 0.4.0: Funcs tab, stat cards. 0.3.0: Mod Book tabs.
- 0.1-0.2: proof of concept (8 live properties).

## License

GNU GPL version 3 (`LICENSE.txt`). Copyright (C) 2026 Latedozer. Other people's parts:
`THIRD_PARTY_NOTICES.md`.
