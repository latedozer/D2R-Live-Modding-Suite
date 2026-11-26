<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE. -->
# Live Export 0.3.4

`d2rl-live-export.dll`: a D2RLoader plugin for Diablo II: Resurrected (game build 3.4.95041, D2RLoader 1.3.1).
It writes the mod you are playing with every change the Live Suite made in it, so the exported mod
works **with the whole suite removed**. It does nothing until asked. It writes only into
`d2rloader/live-suite/mod-export/`; the mod's own folder is only read.

## Use

In the Mod Book tab **Export**, type a name, then press a button. Or use the console (Ctrl + `):

| Command | Writes |
|---|---|
| `export mpq [name]` | `mod-export/<name>/<name>.mpq/`: the whole mod, with Exalted Items' table copies |
| `export mpq [name] plain` | the same, without those copies |
| `export d2rmm [name]` | `mod-export/<name>-d2rmm/`: `mod.json` + `mod.ts`, which edit rows in place on top of other mods |

The name defaults to `<mod>-live`. An export with the same name is replaced. Every export has an
`export-report.txt` that lists what went in and what was left out. Its last line is
`could not carry: ...` (or `could not carry: nothing`).

- **.mpq:** copy `<name>` into the game's `mods` folder and start the game with `-mod <name>`. Keep
  Exalted Items on (1.1.0+): it sees its rows are there and adds only its hooks.
- **D2RMM:** copy `<name>-d2rmm` into D2RMM's `mods` folder, put it after the mods it was made with,
  and install.

## What an export carries

- The cube recipe list. Exalted `[xlt] ` rows you changed are written whole; ones you removed are
  written with enabled 0.
- Property, stat and affix edits. Affix names you typed become strings.
- Stats and properties you made or grew, only if the game has them this start, at the game's
  numbers, under their current names. A made stat's cells are the grown ones with your stat card's
  edits on top.
- Disabled stats: op, op param, op base, op stat1-3, descfunc, dgrp, itemevent1/2 empty.
- Disabled properties: func1-7 empty.
- Disabled affixes: spawnable 0, frequency 0.

**Stat numbers:** a stat's number is its row, and saved items keep numbers. So a made stat is only
ever the next row of the stat table. Live Export never adds filler rows. If its number doesn't fit,
it is refused. The D2RMM mod checks the stat table first and stops without changing anything if
another mod moved the numbers.

## Where its inputs come from

| Source | What |
|---|---|
| Live Properties, `live-suite/live-properties/<mod>/` | property-edits, stat-edits, affix-edits, property-off, affix-off, made-properties, string-keys; API `live_props_api.h` v2 `listMade` |
| Live Tables, `live-suite/live-tables/<mod>/` | grown-rows.txt (cells), disabled-rows.txt; API `live_tables_api.h`: `find`, `count`, `isDisabled` say what the game has |
| Live Cube Recipes, `live-suite/live-cube-recipes/<mod>/` | recipes.txt, recipes-classic-lod.txt |
| Exalted Items | its table copies (.mpq) and its `stat-ids.txt` (to leave out its own rows) |

## Build, test, install

- `./build.sh` builds with mingw-w64 (SDK `../PluginSDK-master`, CascLib from Live Cube Recipes'
  `third_party`). Builds are not byte-reproducible.
- `./test/run_tests.sh`:
  - `export_core` runs natively on the vanilla tables;
  - the `mod.ts` it writes runs in node against a stand-in D2RMM and must give the same tables;
  - Exalted Items' generator runs on both exports.
- `./install.sh` copies the DLL into `d2rloader/plugins`, only when the user says "install". It
  refuses while the game runs. It keeps the old DLL in `Suite .dlls/previous/` and appends
  time, version and sha256 to `d2rloader/live-suite/installed.txt`.
- The log is `d2rloader/logs/live-export.log`. Its first line gives the DLL's version and sha256.

## Known limits

- The .mpq export has not run in the game.
- An affix's class cells (classspecific, class, classlevelreq) are not exported: the game keeps
  them as numbers, the tables as class codes.
- A stat text cell with a string number that has no known key keeps the table's own text. String
  5382 (`dummy`, the game's "no string") is written empty.
- The whole-mod export copies every file of the mod (D2RMM: about 360 MB).

## Changes

- 0.3.4: disabled stats and affixes, Live Tables' grown rows and renames, the "could not carry" line, sha256 in the log, install log.
- 0.3.3: made rows only from Live Properties' listMade; no filler rows; string 5382.
- 0.3.1-0.3.2: made stats placed by number; property-off; button labels checked.
- 0.3.0: action row; Exalted Items 1.3.0's stat-ids.txt.
- 0.2.0: carries edits to Exalted Items' recipes; plain .mpq.
- 0.1.0: first build (.mpq and D2RMM exports).

## License

GNU GPL version 3 (`LICENSE`). Copyright (C) 2026 Latedozer.
