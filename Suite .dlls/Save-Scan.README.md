# Save Scan - version 0.2.0

`d2rl-save-scan.dll`, version 0.2.0. A D2RLoader plugin for Diablo II: Resurrected, made for game
build 3.4.95041 and D2RLoader 1.3.1.

## What it does

Save Scan reads every saved item: all your characters (`.d2s`) and the shared stash (`.d2i`) of
the mod's save folder. It says which items hold a given stat.

It exists for one question that must be asked before a stat's saved shape is changed: "would an
item somewhere stop being readable?"

- It **does nothing until asked**. No hooks, no timers, nothing at start but its console command.
- Saves are **only read**, never written.
- It reads stats with the stat table the game holds in memory at that moment, so stats changed or
  made by Live Properties are read the way the game saves them.

## With and without the rest of the suite

- **With Live Properties**: Live Properties asks Save Scan before every change to how a stat is
  saved, and refuses the change when a saved item holds the stat or a save file could not be read.
  This has been used in the game (15 save files, over 1,100 items read).
- **Alone**: only the console commands below. Mod Book is not used.

## Installing

1. Close the game.
2. Copy `d2rl-save-scan.dll` into `Diablo II Resurrected/d2rloader/plugins/`.
3. Start the game through D2RLoader.

To remove it, delete the DLL.

## Console commands

The console opens with Ctrl + `. These commands are new and have not yet been tried in the game.

| Command | Does |
|---|---|
| `savescan` | reads all save files and says what it found |
| `savescan <stat number> [more numbers]` | the items that hold these stats |
| `savescan dump` | writes every item with all its stats to a file (see Files) |

## Files

| File | What it is |
|---|---|
| `Saved Games/Diablo II Resurrected/mods/<mod>/` | the saves it reads (read only) |
| `d2rloader/live-suite/save-scan/<mod>/items.txt` | written only by `savescan dump`: every item with its stats, then every stat in use with the properties that name it |
| `d2rloader/logs/save-scan.log` | the plugin's log |

It also reads `armor.txt`, `weapons.txt` and `misc.txt` from the mod's files and from the copies
other plugins (such as Exalted Items) keep under `d2rloader`.

## Known limits

- A file counts as read only when every item list in it ends exactly where it must. Anything else
  makes the file **unreadable**, and an unreadable file counts as **not safe**.
- Not seen in any save yet, so not sure to be read right: ears, personalized items, classic and
  Lord of Destruction characters.
- The shared stash of the game without a mod (outside the mod's save folder) is not read.
- If the game is updated to another build, Save Scan refuses to run and says so.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
