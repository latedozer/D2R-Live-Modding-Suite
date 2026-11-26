# Live Cube Recipes

A D2RLoader plugin: the Horadric Cube's recipe list is a file of the plugin's own, and the cube
takes changes **while the game runs** (Save in the recipe book, or type `recipes` in the console).
The mod's files are only ever read. Nothing polls: files are read when the game reads its tables
and on `recipes` / Save.

Version 0.16.7, for D2R 3.4.95041 (image stamp `6ab3782c`) under D2RLoader 1.3. Player guide:
`Live-Cube-Recipes-User-Guide.md`.

## Files (`d2rloader/live-suite/live-cube-recipes/<mod>/`)

| File | |
|---|---|
| `recipes.txt` / `recipes-classic-lod.txt` | the cube's whole list (Reign of the Warlock / classic + LoD); no recipe lines = the game's own list |
| `all-recipes*.txt` | the cube's list as it is now, written at start and after each change (read only for you) |
| `legend*.txt` | the words and codes of recipes: cell words, the mod's item types, items, property codes |
| `deleted*.txt` | defaults you deleted, so Search for new recipes does not bring them back |
| `templates/<name>.txt` | whole lists saved from the book |
| `*.before-rename-<date>-<time>.txt` | the file before a Live Tables rename rewrote it (`templates/before-rename/` for templates) |
| `README.txt` | short help, rewritten at every start |

Settings: `d2rloader/config/live-cube-recipes.toml` (`enabled`, `console`).

## In game

- **Recipe book** (Ctrl+B, `recipebook`; a Mod Book tab): scroll, search by typing, click a
  recipe for its card (Edit, Copy as new, Disable/Enable, Remove), New recipe, Search for new
  recipes, Restore deleted defaults, Templates, Undo/Redo (Ctrl+Z/Y), Save (Ctrl+S) / Discard.
  Nothing reaches the file or the cube before Save. Your recipes are marked `*`; a line the game
  cannot read is marked `[cannot be read]`; a mod with a code and blank min, max and param (it
  gives 0) is marked `[no amount]`.
- **Legend** (Ctrl+L, `recipelegend`): two sections of the Mod Book's Legend (Recipe codes,
  Recipe columns) with "How it works" entries; Insert puts an entry into the cell being edited.
- **Console:** `recipes` reads the files now and reports each line it left out and why.

A recipe line is a `cubemain.txt` line. Descriptions read `result <- ingredients`. Every item and
property code the running game knows can be used: the cells go to the game's own parsers.

## How it works

- The game keeps recipes as 0x16c-byte records per data set. Each line is compiled by the game's
  own parsers (D2RCore `CompileCubeInputField` / `CompileCubeOutputField`, the game's mod and param
  column functions); records are written into the plugin's own buffer and the game's vector is
  pointed at it (marked as not the vector's memory). The game's list is never written.
- At start a sample of the mod's `cubemain.txt` is compiled and compared byte for byte with what
  the game holds; any difference, or unknown game code, switches the plugin off with a log line.
- Addresses and layouts: `plugin.cpp` and `cube_core.h`. `cube_core.cpp` needs no game.
- The start log line gives the DLL's name, version and sha256; the DLL exports `LiveSuiteVersion`.

## With the other suite plugins (all optional)

- **Mod Book** (`../Mod Book/mod_book_api.h`): the book and legend are its tab and Legend sections.
- **Live Properties** (`live_props_api.h`): a code the game's lookup does not know is asked of
  `findProperty` (a property made this session).
- **Live Tables** (`live_tables_api.h`, API 1-3):
  - change listener: Grow -> lines refused for a missing code are read again at once; Shrink /
    Remove -> the list is compiled again (numbers moved); Rename -> recipes.txt, deleted.txt,
    templates and the book's unsaved rows get the new code (dated copy first, a log line a file);
  - remove veto: refused while a recipe line (file on disk, the text the cube took, unsaved book
    rows) names the row or a later one (mod cell = property; param with op 3-26 = stat).

## Build, test, install

```
./build.sh            # build/d2rl-live-cube-recipes.dll (mingw-w64, SDK in ../PluginSDK-master, -lbcrypt)
./test/run_tests.sh   # core vs the game's cubemain.bin; addresses vs the image dump; the DLL under Wine
                      # with stand-ins for the game, loader, Mod Book, Live Properties and Live Tables
./install.sh          # game closed: keeps the old DLL in "Suite .dlls/previous/<name>.<version>.dll",
                      # installs, appends time/version/sha256 to d2rloader/live-suite/installed.txt
./install.sh remove
```

## Known limits

- An item added to the mod's tables during play is known only after the game reads its tables.
- A misspelt modifier inside a cell (`qty=`, `sock=` ...) is ignored by the game without a message.
- A unique or set output needs `ilvl` (or `lvl`), as in `cubemain.txt`.
- The book edits the first list (Reign of the Warlock); the classic file is edited by hand.
- In a TCP/IP game the host's recipes count.

## Changes

- 0.1-0.3 - recipe file of its own, overlaid on the game's list; read on command, no polling
- 0.4 - all-recipes.txt
- 0.5-0.6 - recipe book panel (MapSense technique on loader 1.3.1), own keyboard hook
- 0.7-0.8 - legend.txt and legend panel
- 0.9 - in-game editor with Save / Discard
- 0.10 - recipes.txt is the whole list; Search for new recipes, Restore deleted defaults, templates; `result <- ingredients`
- 0.11 - real buttons (the game's ModalButton art, resized)
- 0.12 - scrolling, keys (Enter, arrows, Ctrl+S/N/V/C), legend Insert
- 0.13 - undo/redo; performance (one split per line, shared compile cache)
- 0.14-0.15 - Mod Book tabs
- 0.16.0-0.16.2 - Mod Book action row and Legend sections; label length check
- 0.16.3 - made properties via Live Properties' findProperty
- 0.16.4 - "[no amount]" note; Live Tables change listener (refused lines read again, recompile on removal)
- 0.16.5 - Live Tables remove veto
- 0.16.6 - renames rewrite recipes.txt and the book; sha256 at start; install.sh keeps the previous DLL
- 0.16.7 - renames rewrite deleted.txt and templates too

## License

GNU General Public License version 3 (`LICENSE` at the top of the repository). Copyright (C) 2026
Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
