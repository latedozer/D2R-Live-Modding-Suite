# Live Cube Recipes - version 0.15.0

`d2rl-live-cube-recipes.dll`, version 0.15.0. A D2RLoader plugin for Diablo II: Resurrected, made
for game build 3.4.95041 and D2RLoader 1.3.1.

## What it does

Live Cube Recipes gives you the Horadric Cube's recipe list as a list of your own. You add, change,
switch off and remove recipes **while you play**. No restart, no Save and Quit. Your list is kept in
a file of the plugin's own; the mod you play is never changed.

- Recipes are written exactly as in the game's `cubemain.txt`, one line each.
- The game itself reads your cells, so every item, item type, unique, set item and property of
  your mod (and of other plugins) can be used.
- After each change the plugin writes out every recipe the cube has, and a legend of all words
  and codes.

## With and without the rest of the suite

- **Without Mod Book**: the recipes work fully through the files and the console. Edit
  `recipes.txt`, save it, type `recipes` in the console, and the cube uses it. There is no recipe
  book panel.
- **With Mod Book** (`d2rl-mod-book.dll`): the recipe book and the legend are tabs in its panels:
  - **Cube Recipes** (left, Ctrl+B): every recipe of your list. Search by typing, click a recipe to
    edit, copy, disable or remove it, make new ones, save, undo, templates.
  - **Recipe Words** (right, Ctrl+L, group "Cube Recipes"): what the codes and words in recipes
    mean: `rar` = rare, `shld` = any shield, `r10` = Thul Rune, property codes.
  - **Recipe Terms** (right, group "General"): every column, every `op` number, stat numbers,
    `lvl` / `plvl` / `ilvl`, the mod cells.
- Live Properties and Save Scan are not needed.

## Installing

1. Close the game.
2. Copy `d2rl-live-cube-recipes.dll` into `Diablo II Resurrected/d2rloader/plugins/`.
   For the in-game book, copy `d2rl-mod-book.dll` there too.
3. Start the game through D2RLoader.

On the first start the plugin makes its folder (see Files) with an empty recipe file, a list of all
recipes and a legend. To remove the plugin, delete the DLL. Your files stay.

## Your list and the game's recipes

`recipes.txt` is **the cube's whole recipe list**.

- While it has no recipe lines, the cube has the recipes of the mod and the game, as without the
  plugin. The recipe book shows them, ready to be changed.
- Once it holds recipes (after your first Save), the cube has exactly the recipes in it, in its
  order. The cube tries them from the top and takes the first that fits.
- **Search for new recipes** (in the book) offers the mod's and game's recipes your list does not
  have, for example after a mod update. **Restore deleted defaults** puts back the ones you removed.
- Nothing changes in the cube until you **Save**.

## Keys

With Mod Book installed:

| Key | Does |
|---|---|
| Ctrl+B | opens and closes the book (the Cube Recipes tab is on the left) |
| Ctrl+L | opens and closes the legend (right) |
| Ctrl+N | new recipe |
| Ctrl+S | save your changes |
| Ctrl+Z / Ctrl+Y | undo / redo (up to 30 back, as far as your last Save) |
| Ctrl+C / Ctrl+V | copy the cell you are in / paste |
| type | search: only recipes with every word you typed stay |
| Escape | back one screen; asks before closing with unsaved changes |

Ctrl+B and Ctrl+L are Mod Book's keys; they can be changed in the game's key settings under
"Mod Book".

## Console commands

The console opens with Ctrl + `.

| Command | Does |
|---|---|
| `recipes` | reads `recipes.txt` again and has the cube use it (after editing it by hand) |
| `recipebook` | opens and closes the recipe book (needs Mod Book) |
| `recipelegend` | opens and closes the legend (needs Mod Book) |

## Files

Everything is in `Diablo II Resurrected/d2rloader/live-suite/live-cube-recipes/<mod>/` (an older `d2rloader/live-cube-recipes/<mod>/` folder is moved there at the first start), where `<mod>` is the
name of the mod you play (`D2RMM` for D2RMM).

| File | What it is |
|---|---|
| `recipes.txt` | the cube's recipe list (Reign of the Warlock characters) |
| `deleted.txt` | the mod's and game's recipes you removed, so a search does not bring them back |
| `templates/` | whole recipe lists saved under a name |
| `all-recipes.txt` | every recipe the cube has right now; written by the plugin, changing it does nothing |
| `legend.txt` | the legend as a text file, written at every start |
| `README.txt` | a short reminder, written at every start |
| `...-classic-lod.txt` | the same files for classic and Lord of Destruction characters |

Settings: `d2rloader/config/live-cube-recipes.toml` (`enabled`, `console`). Close the game before
changing it. Log: `d2rloader/logs/live-cube-recipes.log`.

The file is read when the game reads its tables (at start) and when you type `recipes`. Nothing
watches it in between.

## Good to know

- The recipe book and its editor work on the Reign of the Warlock list. The classic/LoD file can be
  edited by hand.
- A line with a mistake is left out, with a message naming the line and the cell. Your other
  recipes are not affected.
- A misspelt word after a comma (`qty=`, `eth`, `sock=` ...) is ignored by the game without a
  message, exactly as in `cubemain.txt`.
- An item added to the mod while the game runs is known to the game only after the next start.
- A recipe gives at most five properties to each output. That is the game's limit.
- Lists are kept per mod, because item codes differ from mod to mod.
- In a multiplayer (TCP/IP) game the host's recipes count.
- If the game is updated to another build, the plugin switches itself off and the cube has the
  game's recipes. The log says so.
- Property codes made by Live Properties cannot be used in `recipes.txt` yet.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
