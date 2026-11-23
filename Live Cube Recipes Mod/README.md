# Live Cube Recipes

A D2RLoader plugin. Your own Horadric Cube recipes live in a file **of the plugin's own**, and the
cube takes them in **while the game runs**: add, change or delete a line, save, type `recipes` in the
console, and the cube uses it. No restart, no Save and Quit. The recipes are also there at every later start.

The file is read at two moments only: when the game starts (reads its tables) and when you type
`recipes`. Nothing watches or checks it in between.

The mod's files (`D2RMM.mpq` or any other mod) are only ever read, never written.

Version 0.13.1, written for D2R 3.4.95041 (image stamp `6ab3782c`) under D2RLoader 1.3.
It runs in the real game: the recipe book, the legend with its tabs, and the editor (new recipes,
changes and disabling saved from the book and taken by the cube) were used there on 2026-11-21.

## Since 0.10: recipes.txt is the whole list

Up to 0.9, `recipes.txt` held changes that were laid over the game's list. From 0.10 it is the
cube's whole list (a `*complete` column in its first line says so):

- an empty file = the game's own list (the defaults: the mod's and the game's recipes);
- a file with recipes = the cube has exactly these, in this order, plus any record of the game
  that no recipe file has text for (such a record cannot be written into a file);
- a file of the old kind is rewritten once as the whole list with the same result, the old file
  kept as `recipes.txt.before-0.10`, its removals noted in `deleted.txt`;
- **Search for new recipes** adds defaults whose ingredients the list does not have and that are
  not noted in `deleted.txt`; **Restore deleted defaults** adds the noted ones and clears the notes;
  both ask first and need Save;
- **Templates** (`templates/<name>.txt`) are whole lists saved from the book; loading and deleting
  ask first;
- removing a recipe asks first (0.10.1);
- the recipe line of the book reads result first (`r11  <-  r10,qty=3 + gcy`), the editor's
  "Name it from its cells" (`cube::GenericName`) makes a description in that order (0.10.2), and
  descriptions written `A + B -> C` are shown, and saved, as `C <- A + B` (`cube::ResultFirst`, 0.10.3).

The sections below that speak of "a line with the same ingredients stands in the game's recipe's
place", `*action` = `remove` and `[removed]` entries describe 0.9 and are kept for the history of
the design; `Live-Cube-Recipes-User-Guide.md` is the description of how it works now.

0.11.0: buttons are real buttons. `ButtonSetUp` reads `PANEL\Modals\ModalButton` (the mod's loose
copy, else the game's CASC data through CascLib in `third_party/`), `cube::ButtonSprite` resizes it
to each width in `kButtonWidths` (44 high; half size for `.lowend.sprite`), and the pictures are
registered as `d2rloader\live-cube-recipes\button<width>`; the layout's ButtonWidget carries the
text (`textString`). No art of the game is shipped. A new button width must be added to
`kButtonWidths`, or that button falls back to `[ words ]`.

0.12.0: lists scroll by entry (`Book::top`, `Editor::fieldTop`; wheel = `kWheelRows`), with a
scroll line (`BarSetUp`, `LayoutText::bar`). Keys travel in order through `Book::keys` (`kKey...`):
Enter, Shift+Tab, Up/Down, Ctrl+S/N/V/C (the hook takes only these four Ctrl keys). The editor
shows the usual and the filled cells (`EditorFields`, `Editor::allCells`). A click on a legend
row calls `LegendInsert`.

0.13.0: undo and redo. `EditorKeep(at, before, after)` is called before every change and keeps
the rows about to be replaced plus the deletion notes (`Editor::Step`, 30 deep); `EditorUndo`
swaps a step back in and pushes its inverse on the other stack. Save, Discard and a re-read empty
both.

0.13.1: performance, no change in behaviour. A line is split into cells once (`SplitCells`; `Cell()`
walked the line anew for each of 104 columns). The compile cache is keyed by the cells a record
depends on (`RecordKey`), so the defaults, the user's list and `all-recipes.txt` share one entry a
recipe, whatever the header and description. The book keeps a `DefaultsIndex` and a memo by line
text (`Bank::rowMemo`), so a change works out one line (`FactsOf`, `Summary`). Templates are
counted, not parsed. The lists the book does not show drop their compile caches after start-up.
On a 4127-recipe list: book rebuild after a change 125 ms -> 4 ms, first open about 460 -> 35 ms.

Core: `cube::ExactList`, `cube::Compare`, `cube::CompleteHeader`. Plugin: `ReadDefaults`,
`Migrate`, the editor's `View::Confirm`, `View::Templates`, `View::TemplateName`.

## Where the recipes are

```
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/recipes.txt               Reign of the Warlock characters
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/recipes-classic-lod.txt   classic and Lord of Destruction characters
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/all-recipes.txt               every recipe the cube has right now
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/all-recipes-classic-lod.txt   the same for the older data sets
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/README.txt                the short version of this page
```

For D2RMM, `<mod>` is `D2RMM`. The folder and the files are made on the first start, each with only
the first line (the column names of the game's `cubemain.txt`). Open them with the editor you use
for the game's `.txt` tables. A D2RMM Install does not touch them.

## The recipe book (in game)

Press **Ctrl+B** in a game (or type `recipebook` in the console) to open a panel with every recipe
the cube has: 10 a page, each with its description and, under it, its ingredients and result as
they are written in the recipe files (`r10,qty=3 + gcy  =>  r11`).

- **Scroll:** the mouse wheel, **Page Up / Page Down** or the arrows at the bottom turn the page;
  **Home / End** go to the first and last page.
- **Search:** just type. Only recipes containing every word stay (in the description, an item
  code, an output). **Backspace** erases a letter, **Delete** clears the search.
- **Whole recipe:** hold the cursor over a recipe; a hover box shows its full description and
  its full ingredients and result (rows cut long lines with "...").
- **Close:** Escape (goes back one screen first), Ctrl+B or `recipebook`. The key can be changed in the game's key settings
  ("Cube Recipe Book").
- After a `recipes` command an open book shows the new list at once.

While the book is open the plugin takes the keys that are text, the paging keys and the wheel, so
the game's hotkeys on those keys do not fire; everything else (Escape, F keys, Ctrl and Alt
combinations, the mouse buttons) stays the game's. Closing the book gives the keys back.

It is a panel of the game's own kind (the side panel frame the quest log uses, the stash's page
arrows) and shows the list for Reign of the Warlock characters. D2RLoader 1.3.1 cannot change the
text of an open panel, has no text entry for plugin panels and no key listener, so the book works
the way RuffnecKk's MapSense does on this loader: every page is a panel of its own from a pool of
24 (the next is opened, then the last closed), and the keyboard and wheel are read with a hook of
the plugin's own while the book is open.

## Adding, editing, disabling and removing recipes in game

The recipe book is also an editor for your `recipes.txt`.

- **New recipe** (top of the list) opens the cell editor on an empty recipe.
- **Click a recipe** to open its card: every cell it has, and
  - **Edit**: change its cells. Editing a recipe of the game makes your version of it;
  - **Copy as new**: start a new recipe from it;
  - **Disable / Enable**: it stays in the list, switched off (the cube skips it);
  - **Remove**: it is taken out of the cube's list. It stays listed at the end as `[removed]`,
    where **Put back** undoes it;
  - **Game's own** (on your version of a game recipe): drops your version.
- **The cell editor:** click a cell and type; Backspace erases, Delete empties the cell, Tab goes
  to the next; the arrows or the wheel turn to more cells. Cells are written as in
  `cubemain.txt` (open the legend beside it). **Done** checks the recipe with the game: a cell
  the game cannot read is named and you stay in the editor. A blank `numinputs` is counted for you.
- Recipes of yours are marked `*` in the list (search `*` to see only those).

**Nothing reaches `recipes.txt` or the cube until you save.** While there are changes the list
shows **Save N changes** and **Discard**. Save writes `recipes.txt` and the cube uses it at once.
Closing the book (Ctrl+B or Escape) with unsaved changes asks first: Save, Discard the changes, or
Keep editing.

The editor works on `recipes.txt` (Reign of the Warlock characters). Saving writes the file in the
plugin's own column order.

In the file itself the same things are: a line with `enabled` 0 = disabled; a line with `remove`
in the `*action` column = removed.

## The legend (in game)

Press **Ctrl+L** in a game (or type `recipelegend` in the console) to open the legend as a panel on
the **right** side of the screen, opposite the recipe book. It has the same entries as `legend.txt`:
the recipe words, every item type, item and property code. Type a word or code (`rar`, `shld`,
`r10`, `thul`) to look it up; the wheel and Page Up / Page Down turn its pages; hold the cursor
over an entry to see it whole.

The legend has two tabs, picked at the top of the panel:

- **Words and codes:** the words of input and output cells (`rar`, `useitem`), item type codes,
  item codes, property codes.
- **Columns and values:** every column and what its values mean: each `op` number (`op 18`), what
  `param` and `value` are for it, the stat numbers `param` refers to (`stat 370`), `enabled`,
  `min diff`, `version`, `class` codes, `lvl` / `plvl` / `ilvl`, and the cells of `mod`, `b mod`
  and `c mod`.

With both panels open, typing, the page keys and the wheel go to the panel the cursor is over
(left half of the screen: recipe book; right half: legend).

## legend.txt

Written at every start, beside your recipe file: what the words and codes in recipes mean.

1. The words of an input and an output cell (`rar` = rare, `nor` = normal, `qty=3`, `useitem`,
   `usetype`, `sock=`, ...).
2. Every item type code of the mod with its name, what it is a kind of and which types it
   includes (`shld` = Any Shield, includes `ashd, grim, head, shie`).
3. Every item code with its name and type (`r10` = Thul Rune).
4. Every property code (the `mod` columns).

Lists 2 to 4 are read from the mod's own tables (and other plugins' copies of them), so they follow
whatever the mod adds. `legend-classic-lod.txt` is the same for the older data sets.

## all-recipes.txt

At every start, and after every `recipes` command, the plugin writes the cube's whole list into
`all-recipes.txt`: the game's recipes, other plugins' recipes and yours, in the order the cube tries
them, with the same columns as `recipes.txt`. Look recipes up there and copy lines from it into
`recipes.txt`. Changing `all-recipes.txt` itself does nothing; it is written over.

The game keeps recipes as numbers, not text. The text of each recipe is therefore taken from the
recipe file it came from: your file, the mod's `cubemain.txt`, and copies of it that other plugins
keep in the loader folders (Exalted Items does). All of these are only read. A recipe no file has a
line for is still listed, with a description saying so and empty cells; the log counts them.

## Using it

One line is one recipe, written exactly as in `cubemain.txt`. After saving, open the console
(Ctrl + `) and type `recipes`.

| You want to | Do this |
|---|---|
| **add** a recipe | Write a line with ingredients no recipe of the game has. It goes in front of the game's recipes (the cube takes the first recipe that fits). |
| **change** a recipe | Copy its line from `all-recipes.txt` into your file and change the output, mods or levels. A line with the same ingredients as a recipe of the game stands in that recipe's place. |
| **disable** a recipe | Copy its line into your file and set `enabled` to `0`. It stays in the list, switched off. |
| **remove** a recipe | Copy its line into your file and write `remove` in the `*action` column. |
| **undo** any of these | Delete the line from your file. |

"The same ingredients" means the same in every column from `enabled` to `input 7` (`enabled` itself
aside): `min diff`, `version`, `op`, `param`, `value`, `class`, `numinputs`, the ladder seasons and
the inputs. To change a recipe's *ingredients*, remove the old one (enabled 0) and add the new one.

**Every item the running game knows can be used**: item codes, item types, unique and set item
names and property codes are looked up by the game's own recipe parser in the tables it has
loaded, so items of the mod and items other plugins add (Exalted Items' bases and materials, for
example) work without any list to keep up to date.

The console (Ctrl + `) says what happened, and so does `d2rloader/logs/live-cube-recipes.log`:

```
Live Cube Recipes: recipes.txt: 1 new recipe, 1 recipe in place of the game's, 1 removal. The cube now knows 3105 recipes.
Live Cube Recipes: recipes.txt line 4 (My new recipe): input 2: the game does not know "xyz"
   (not an item code, an item type or a unique/set item name) - this line is left out until it is fixed
```

### Good to know

- A line with an error is left out until it is fixed; nothing else is affected.
- A misspelt *modifier* inside a cell (`qty=`, `eth`, `sock=` ...) is ignored by the game itself,
  without a message, exactly as in `cubemain.txt`.
- An item added to the mod while the game runs is known to the game only after its tables are read
  again (next start, or D2RLoader's `reload`). Your file is laid over the new tables then, by itself.
- For a unique or set item as output, give `ilvl` (or `lvl`) a value, as mods do in `cubemain.txt`.
- Recipes are kept per mod, because item codes differ between mods.
- In a TCP/IP game the host's recipes count.

## Settings

`d2rloader/config/live-cube-recipes.toml` (made on the first start):

| Setting | Default | |
|---|---|---|
| `enabled` | `true` | `false` = the plugin does nothing |
| `console` | `true` | a console line for every change and every line left out |

## How it works

- The game keeps its recipes as a list of 364-byte records, one list per data set. The cube walks
  that list on every transmute.
- The plugin has no thread of its own and does nothing between commands. `recipes` reads the files
  and hands them to the game's own thread (the UI's when no game is running). Each line is made into a record there: the input and
  output cells by **the game's own parsers** (the functions its table loader calls), property codes
  by the game's own column function, numbers as numbers.
- Your recipes are laid over a copy of the game's list, the result is written into memory of the
  plugin's own, and the game's list is pointed at it, marked the way the game marks memory it must
  not free. The game's original list is never written to.
- Safety: at start the plugin compiles a sample of the mod's own `cubemain.txt` (read only) and
  checks that the result is byte for byte what the game holds; the log says so. If it is not, or if
  the game's code or D2RLoader's recipe layout is not what the plugin was written for, the plugin
  switches itself off and says why in the log.

`plugin.cpp` has the addresses; `cube_core.cpp` is the part that needs no game.

## Building and testing

```
./build.sh              # build/d2rl-live-cube-recipes.dll (mingw-w64, SDK headers in ../PluginSDK-master)
./test/run_tests.sh     # all checks
./install.sh            # copies the DLL into <game>/d2rloader/plugins (game closed)
./install.sh remove
```

The tests: the game's own `cubemain.txt` (382 lines, both data sets) compiles to the records in
the game's own `cubemain.bin`; the overlay (new, in place, removed, bad lines); every address in
`plugin.cpp` against the dump of the game image; and the real DLL under Wine in a stand-in for the
game and the loader, which also checks that nothing in the mod's folder is ever added, removed or
changed.

`backup-src-0.1.0/` is the first version, which followed the mod's own `cubemain.txt` instead;
`backup-src-0.2.0/` checked the recipe files twice a second instead of on command.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE` at the top of the repository). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `Live Cube Recipes Mod/THIRD_PARTY_NOTICES.md`.
