<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE. -->
# Live Tables 0.3.4

`d2rl-live-tables.dll` grows and edits the ItemStatCost and Properties tables **while the game
runs**. The work happens in the game's memory, with the game's own routines. There is no reload and
no restart, and the mod folder is never written. Other suite plugins use it through
`live_tables_api.h`.

## What it does

- **Grow a row.** The work runs in a game job (at the main menu: a UI job), and nothing waits on
  another thread.
  - The row is compiled the way the game's row compiler 0xa12770 does it. The name goes in with the
    game's insert 0xa11e10, lookups use 0xa121f0, string keys use 0x308be0.
  - Old rows stay as they are. Older rows that name the new name get its number.
  - Stat op links go where a load would put them, and D2RCore's `BuildItemStatDescriptionOrder`
    rebuilds the tooltip order.
  - The stat's name is added to D2RCore's stat-name lists (under its own lock, with its own heap),
    so a save written now matches the next start.
  - New memory comes from the game's allocator. The old buffer stays valid until the next table load.
  - A growth that would rebuild a name table's buckets is refused.
- **Keep it for the next start.** Grown rows are kept as text in `grown-rows.txt` and laid into the
  table's text every time the game reads it. This hook on D2RCore 0x29a560 is gated only on its own
  fingerprints.
  - A row already in the text at its own number is skipped ("already in the mod's table").
  - A made stat found at another number raises the moved alarm (below).
- **Disable / enable a stat.** The stat has no effect but is still saved. Kept in `disabled-rows.txt`.
- **Remove a grown row.**
  - Refused while anything still names it: another stat, a property, a recipe, an affix, a unique,
    a runeword, or a table read at load.
  - The last row in a game is refused until the item walk is proven in a log.
  - A middle row is removed at the main menu only, in one job: every check first, then Save Scan
    renumbers the saves (dated backup), then `grown-rows.txt`, then the tables.
  - A journal, `remove-in-progress.txt`, lets the next start finish the remove or put the saves back.
- **Rename a grown row.** The name tables, D2RCore's list and the kept rows follow.
- **Cells of a grown row (`setCells`).** The row is compiled again and kept as text (dated copy
  first). Desc cells rebuild the tooltip order.
  - Save bits / save add / save param bits that differ from the row **in memory** go through Save
    Scan's resize first, at the menu only. A value memory already holds is only recorded.
  - Other save cells (signed, valshift, saved, 1.09-*, csv*) are refused while a save holds the stat.
- **Moved-number alarm.** A made stat at another number this start (a mod update, an exported mod)
  raises an alarm. Every stat change is refused until `tables fixmoved` at the menu has Save Scan
  renumber the saves.
- **At start.** If a made stat's signed / send bits / save bits / save add in `grown-rows.txt`
  differ from Live Properties' `stat-edits.txt`, the `stat-edits.txt` value is taken. A dated copy is
  made first and each change is logged.
- **Threads.** Remove, rename and setCells are refused when the game and UI threads differ.
- **Start log.** It gives the name, version and the DLL's sha256.

## API (`live_tables_api.h`, version 5)

`LiveTablesGetApi(v)` returns the newest Api to any caller asking for v >= 1. Check `api->size`
before using the later fields.

- `check`, `grow`, `count`, `limit`, `find`, `revision`: rows and growth.
- `addListener` / `removeListener`: Change kinds Grow, Load, Disable, Enable, Shrink, Remove,
  Rename, Alarm, CellEdit.
- `disable`, `enable`, `isDisabled`, `remove`, `rename`, `setCells`: queued, then answered through
  `done`.
- `addVeto` / `removeVeto`: another plugin may refuse a remove.
- `status`, `moved`, `fixMoved`: the moved-number alarm.
- `list(table, dv, callback, user)`: the kept rows (name, number, cells, disabled), read from the
  files, from any thread.

Console (Ctrl + `): `tables`; `tables grow stat|prop column=value;...`;
`tables disable|enable|remove stat <name>`; `tables remove prop <code>`;
`tables rename stat|prop <old> <new>`; `tables fixmoved`; `tables status`.

## Files (`d2rloader\live-suite\live-tables\<mod>\`)

- `grown-rows.txt`: one line per grown row, `table  v=<data versions>  n=<number>  [nb=<base number>]  column=value...`.
- `disabled-rows.txt`: `itemstatcost  v=<data versions>  <stat name>`.
- `remove-in-progress.txt`: the journal of a remove or a save resize.
- `grown-rows.before-*.txt`: dated copies made before each change.

## Limits

- Stats: 511 (0–510) with the game's own save format, 32768 with D2RCore's wide format.
- Properties: 32767.
- A stat keeps at most 16 op links from other stats. When a new link belongs before the 16th, the
  highest one drops out, as it would at load. Its flag bits at +4 are not recomputed.
- Rows in other tables (skills, states, items, monpet, formulas) that name a stat before it is grown
  stay unresolved. A row such a table names cannot be removed or renamed.
- Last-row remove in a game is refused until the item walk is proven.

## Build, test, install

- `./build.sh` builds the DLL (mingw-w64, SDK `../PluginSDK-master`, `-lbcrypt`) and
  `build/grow_test`, `build/name_list_test`, `build/names_test`.
- `./test/run_tests.sh` checks:
  - the game's hash against the port (run from the image dump);
  - vanilla, mod and Exalted tables grown in the model against the same text loaded;
  - D2RCore's stat-name list;
  - kept numbers, the journal steps and the stat-edits reconcile;
  - the read hook's name scan;
  - the fingerprints against the dump and D2RCore.dll.
- `./install.sh` only when the user says "install"; it refuses while the game runs. It copies the
  old DLL to `Suite .dlls/previous/` and appends a line to `live-suite/installed.txt`.

Findings and addresses: Claude memory `live-tables-plugin`.

## Changes

- 0.3.4: disable/enable report and use each data version's own number (was a write past a list); a remove journal is kept when grown-rows.txt cannot be read; a save resize needs one number in every data version.
- 0.3.3: send bits are taken from `stat-edits.txt` at start too.
- 0.3.2: atomic middle remove with journal; hook gated on its own fingerprints; moved alarm and
  fixMoved; setCells; the stat-edits reconcile; list(); thread refusal; sha256 in the log; install
  ledger.
- 0.3.1: the loaded character's items are read in a UI job; numbers kept per data version (`nb=`).
- 0.3.0: rename; Properties remove; remove vetoes.
- 0.2.1: the name scan of other tables' texts.
- 0.2.0: disable, enable, remove.
- 0.1.3: no cross-thread wait (the game and UI threads are one thread in this install).
- 0.1.1: D2RCore stat-name list grown with the stat.
- 0.1.0: grow ItemStatCost and Properties.
