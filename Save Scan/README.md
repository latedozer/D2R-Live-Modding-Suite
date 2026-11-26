# Save Scan

A D2RLoader plugin (`d2rl-save-scan.dll`) for the saves of the mod being played: every character
(`.d2s`) and the shared stash (`.d2i`). It reads every saved item when asked (which items hold a
stat or an affix), rewrites the saves when a stat or affix row is removed, renumbered or resized,
and at the main menu warns about saves that are not safe to load. No hooks, no timers.

Build `./build.sh`, test `./test/run_tests.sh` (native, on copies of the saves in `build/`, never
committed), install `./install.sh` (refuses while the game runs; keeps the replaced DLL in
`Suite .dlls/previous/`, adds time, version and sha256 to `d2rloader/live-suite/installed.txt`).

## API (`save_scan_api.h`, Api version 3; `savescan::Find(minVersion)`)

- `scan(stats)` / `scanAffixes(ids)`: the saved items that hold them; `savescan::Safe(summary)`.
  Caller's thread (game or UI), reads the game's stat table in memory.
- `dump`: every item and stat into `items.txt`.
- `fixSaves(FixRequest)` (async, done() on Save Scan's thread) / `fixSavesNow` (caller's thread):
  `changes` {from, to | kDropped}, `resizes` (new save bits / param bits / save add),
  `affixChanges`, `before` (the shapes the saves were written with), `notAfter`, `kDryRun`.
  Call BEFORE the table changes. Promise: ok = 1 every changed file written, read back, checked;
  ok = 0 every file as before (else `notRestored` > 0, said in `why`). `why` is a whole sentence.
- `restore(backupFolder)`: puts back only the files that fix wrote, each only if still as written
  (safe twice; a file saved since is left, named).
- `SaveScanGetApi(v)` gives the newest table for any v >= 1; callers check version and size.

## How a fix works

1. Read every file with the current table (with `before` applied); one unreadable file: refused.
2. Rewrite in memory: each item's stat lists written again (new ids, values re-encoded, dropped
   entries left out), affix ids rewritten; everything else copied bit for bit; then the `.d2s`
   size + checksum and the `.d2i` section lengths. Read back with the table after the change and
   compared item by item; every byte outside the items must be unchanged.
3. Dry run: stop here. Else: no character may be in a game (checked again before the backup and
   before every write); dated backup with every save file (read back); `written.txt` (each file,
   size, FNV-1a hash of what is written); then each file written beside itself, moved into place,
   read back, compared. Any failure: every file written is put back. `report.txt` ends a fix.

Refused: a character in a game (dry run allowed); moving or dropping stats the game saves together
(17-18, 48-55, 57-59); a change to a stat in a character's attributes (`gf`); ids past 510 or two
stats on one number; a value that does not fit a new shape; a magic item left with neither prefix
nor suffix; a file saved after `notAfter`; a character whose closing `lf` count is not 0; a stash
record of unknown shape. "signed" does not change the saved encoding.

## At the main menu (when the game has read its tables)

Looks for a fix that was cut off (a backup with `written.txt` but no `report.txt` / `restored.txt`)
and for saves holding stats Live Tables made that the game's table lacks now (Live Tables missing
or its rows off). Says so in the log, the console and its Mod Book tab "Saves" (with "Put the
backup back").

## Console

`savescan` (count), `savescan <stat> [...]`, `savescan dump`, `savescan remove <stat>` (dry run of
a middle-row removal, into the log), `savescan check`, `savescan restore <backup folder>`.

## Files (`d2rloader\live-suite\save-scan\<mod>\`)

`items.txt` (dump); `backup-YYYYMMDD-HHMMSS\` = the save files, `where-each-file-came-from.txt`,
`written.txt`, `report.txt`, `restored.txt`. Saves are written only by a fix or a restore.

## Known limits

- Only the current mod's save folder (not the base-folder stash or other folders).
- The loaded character and stash are not fixed in memory yet (a real fix is refused in a game).
- A file's data version is the first stat table that reads it, not the save's own flag.
- Never run in game: the writer (0.3.x), the Mod Book tab, the menu check.
- Unconfirmed layout parts (no sample in the user's files): ears, personalized names, items
  without the 128-bit id, the 55-bit flag-28 block order, classic/LoD characters, `lf` count > 0.
  Layout notes: `save_core.h`. A file counts as read only when every list ends on its marker.

## Changes

- 0.1.0 - the reader (scan, dump), version 105 item layout from the user's saves.
- 0.2.0 - files in `d2rloader\live-suite\save-scan`.
- 0.3.0 - the save writer `fixSaves`: drop, renumber, resize; backup, read back, restore.
- 0.3.1 - refuses stashes with bytes in the record tail; `lf` count > 0 named.
- 0.3.2 - tail accepted when it holds only simple items.
- 0.3.3 - any tail kept (the game counts its record header twice: 64 stale bytes); affix ids
  (`affixChanges`, `scanAffixes`).
- 0.3.4 - `fixSavesNow`, `restore`, write plan in the backup, game check before every write,
  menu alarms, `before` + `notAfter`, GetApi for any version, sha256 in the start line, install log.

## License

Free software under the GNU General Public License version 3 (`LICENSE` at the top of the
repository). Copyright (C) 2026 Latedozer. Other people's parts: `Live Cube Recipes Mod/THIRD_PARTY_NOTICES.md`.
