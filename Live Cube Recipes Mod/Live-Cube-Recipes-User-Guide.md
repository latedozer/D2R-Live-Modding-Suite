# Live Cube Recipes: User Guide

Version 0.13.1, for Diablo II: Resurrected 3.4 with D2RLoader 1.3.

Live Cube Recipes gives you the Horadric Cube's recipe list as a list of your own: you add, change,
switch off and remove recipes **while you play**, look every recipe up in game, and keep several
lists as templates. Your list is kept in a file of the plugin's own. The mod you play (D2RMM or any
other) is never changed.

## Contents

1. [Installing](#1-installing)
2. [Your list and the game's recipes](#2-your-list-and-the-games-recipes)
3. [The recipe book](#3-the-recipe-book)
4. [Adding and changing recipes](#4-adding-and-changing-recipes)
5. [Saving](#5-saving)
6. [Search for new recipes, Restore deleted defaults](#6-search-for-new-recipes-restore-deleted-defaults)
7. [Templates](#7-templates)
8. [The legend](#8-the-legend)
9. [How a recipe is written](#9-how-a-recipe-is-written)
10. [The files](#10-the-files)
11. [Editing recipes.txt by hand](#11-editing-recipestxt-by-hand)
12. [Keys and console commands](#12-keys-and-console-commands)
13. [Settings](#13-settings)
14. [Good to know](#14-good-to-know)
15. [If something goes wrong](#15-if-something-goes-wrong)

---

## 1. Installing

1. Close the game.
2. Put `d2rl-live-cube-recipes.dll` into `Diablo II Resurrected/d2rloader/plugins/`.
3. Start the game through D2RLoader.

On the first start the plugin makes its folder (section 10) with an empty recipe file, a list of all
recipes and a legend.

To remove the plugin, delete the DLL. Your files stay where they are.

---

## 2. Your list and the game's recipes

`recipes.txt` is **the cube's recipe list**.

- **While it is empty**, the cube has the recipes of the mod and the game, as without the plugin.
  The recipe book shows them, ready to be changed.
- **Once it holds recipes** (after your first Save), the cube has exactly the recipes in it, in its
  order, and nothing else. The game's own recipes are no longer loaded on top.

The recipes of the mod and the game are called the **defaults** in this guide. They never go away:
the recipe book can always fetch them again (section 6).

If you used a version before 0.10: your `recipes.txt` then held only your changes. On the first
start with 0.10 it is rewritten as the whole list, with the same result in the cube. The old file is
kept beside it as `recipes.txt.before-0.10`.

---

## 3. The recipe book

Press **Ctrl+B** in a game. A panel opens on the left with every recipe of your list.

Each recipe has two lines: its description, and under it the recipe itself in the words of recipe
files. Both read **what comes out first, then what goes in**:

```
Amn Rune <- 3 Thul Runes + 1 Chipped Topaz
    r11  <-  r10,qty=3 + gcy
```

Mods usually write descriptions the other way round (`3 Thul Runes + 1 Chipped Topaz -> Amn Rune`).
The book turns those round for you. A description that has no ` -> ` in it is shown as it is.

| To | Do |
|---|---|
| scroll | mouse wheel (two recipes a notch), Up / Down arrows (one recipe) |
| go a screenful | Page Up / Page Down, or the arrows at the bottom |
| go to the start / end | Home / End |
| start a new recipe | Ctrl+N |
| save your changes | Ctrl+S |
| take the last change back / make it again | Ctrl+Z / Ctrl+Y |
| paste into the search | Ctrl+V |
| search | just type; only recipes with every word you typed stay |
| erase a letter / clear the search | Backspace / Delete |
| see a long recipe in full | hold the cursor over it |
| open a recipe | click it |
| go back one screen, close the book | Escape (Ctrl+B also closes) |

A thin line down the right side with a mark on it shows where you are in the list.

Recipes that you changed or made are marked with `*`. Type `*` in the search to see only those.

While the book is open, the keys you type go to the book and not to the game, so typing a letter
does not set off its hotkey. Closing the book gives the keyboard back.

---

## 4. Adding and changing recipes

### A new recipe

Click **New recipe** at the top of the recipe book. The cell editor opens (see below). A new recipe
is put at the front of the list.

### An existing recipe

Click a recipe in the list. Its card shows every cell it has, and these choices:

| Choice | What it does |
|---|---|
| **Edit** | Change its cells. |
| **Copy as new** | Start a new recipe from a copy of it. |
| **Disable** / **Enable** | The recipe stays in the list but is switched off: the cube skips it. |
| **Remove** | Asks first. Then the recipe is taken out of the list. If it is one of the defaults, that is noted, so that a search for new recipes does not bring it back (section 6). |
| **Back** | Back to the list. |

### The cell editor

A recipe is a row of cells: `description`, `enabled`, `numinputs`, `input 1` to `input 7`,
`output`, and so on. The editor first shows the cells nearly every recipe uses, plus every cell
this recipe has filled in. **All cells** shows every cell; **Fewer cells** goes back.

- **Click a cell**, then type. The cell you are typing in is white and ends with `_`.
- **Backspace** erases a letter. **Delete** empties the cell.
- **Tab** or **Down** goes to the next cell, **Shift+Tab** or **Up** to the one before.
- **Ctrl+V** pastes from the clipboard (the first cell of what was copied, so a cell copied from a
  spreadsheet works). **Ctrl+C** copies the cell you are in.
- **Click a legend entry** (open the legend with Ctrl+L) to put its code into the cell you are
  in: look up `thul`, click it, and `r10` is in the cell. In input and output cells the code is
  added after a comma (`shld` then `rar` gives `shld,rar`); other cells take the code as their
  whole content. In the Recipe columns section only `op` and `stat` entries put something in:
  their number.
- The wheel and Page Up / Page Down scroll the cells.
- **Enter** is Done. **Ctrl+S** is Done and Save in one.
- **Done** checks the recipe with the game. If the game cannot read a cell (an item code that does
  not exist, for example), the editor says which cell and why, and you stay in the editor.
- **Name it** writes the description for you, in the same order: what comes out,
  then what goes in, with the mod's names for the codes, for example
  `Amn Rune <- 3 Thul Rune + Chipped Topaz`. You can still change it by hand.
- **Cancel** (or Escape) leaves without keeping what you typed.

Three things are filled in for you if you leave them blank: `enabled` becomes 1, `numinputs` is
counted from your inputs, and the description is made as "Name it" makes it.

A recipe needs at least `input 1` and `output`. Section 9 explains how cells are written; the
legend (section 8) has every word and code.

---

## 5. Saving

**Nothing changes in the cube until you save.** The book shows your changes right away so that you
can see the result, but they are only in the book.

While there are changes not saved, the top of the list shows:

- **Save (N)** (N = the changes not saved yet): writes the list to `recipes.txt`. The cube uses it at once.
- **Discard**: forgets them.
- **Undo** (beside the search box, or Ctrl+Z): takes back the last change only: an edit, a new
  recipe, a removal, a disable, a search or restore, a loaded template. Press it again for the
  change before, up to 30 back. Ctrl+Y makes a change taken back once more.

Undo reaches as far as your last Save. What is saved is in the cube and cannot be taken back.

On the screens that ask a question, **Enter** presses the confirming button and **Escape** the
other one.

If you close the book (Ctrl+B or Escape) with changes not saved, it asks:

- **Save**
- **Discard the changes**
- **Keep editing**

The saved list is the cube's list every time you start the game.

---

## 6. Search for new recipes, Restore deleted defaults

Two buttons under the top row of the recipe book. Both ask before they change your list, and what
they add still needs **Save**.

### Search for new recipes

Looks through the recipes of the mod and the game for recipes that your list does not have, and
offers to add them at the end of your list.

Use it after the mod was updated, or after you installed a mod or plugin that brings recipes.

It leaves out the defaults that you removed. Those are noted in `deleted.txt`.

### Restore deleted defaults

Offers to put back every recipe of the mod and the game that you removed, at the end of your list,
and clears the notes.

### What counts as "the same recipe"

A default counts as being in your list when your list has a recipe with the same ingredients: the
same `min diff`, `version`, `op`, `param`, `value`, `class`, `numinputs`, ladder season columns and
inputs. So a default whose output or properties you changed is not offered again.

---

## 7. Templates

A template is a whole recipe list kept under a name. Click **Templates** at the top of the recipe
book.

| Choice | What it does |
|---|---|
| **Save this list as a template** | Asks for a name (letters, digits, spaces, `-` and `_`) and keeps the list as it is in the book right now, unsaved changes included. A template of the same name is replaced. |
| **Load** | Asks first. Then the template's list takes the place of the list in the book. The cube has it once you save. |
| **Delete** | Asks first. Then the template's file is removed. The book and the cube are not touched. |

Templates are files in the `templates` folder (section 10); you can copy them to share a list.

---

## 8. The legend

Press **Ctrl+L** in a game. The Mod Book's Legend opens on the right, opposite the recipe book, on
the recipe sections. They explain what the words, codes and numbers in recipes mean. Type to
search; the wheel or Page Up / Page Down turn pages. Each row is one line; the longer explanations
are the **How it works** entries at the top of each section.

It has two sections:

**Recipe codes**

- the words of input and output cells: `rar` = rare, `nor` = normal, `qty=3`, `useitem` ...
- item type codes: `shld` = Any Shield, `tors` = body armor ...
- item codes: `r10` = Thul Rune ...
- property codes, used in the `mod` cells: `res-cold` ...

**Recipe columns**

- every column of a recipe and what its values mean
- every `op` number, and what `param` and `value` are for it
- the stat numbers `param` refers to: `stat 370` ...
- `lvl`, `plvl`, `ilvl`, and the cells of `mod`, `b mod` and `c mod`

The item types, items, properties and stats are read from the mod you are playing, so whatever the
mod adds is in the legend.

With both panels open, typing, the page keys and the wheel go to the panel your cursor is over: the
left half of the screen is the recipe book, the right half the legend.

Example: a recipe is described as `Ancients' Pledge - rare - shld - RalOrtTal`. Type `rar` in the
legend: *rare (yellow name)*. Type `shld`: *Any Shield; includes: ashd, grim, head, shie*. So the
recipe wants a rare shield of any kind, with the runes Ral, Ort and Tal.

---

## 9. How a recipe is written

Recipes are written exactly as in the game's `cubemain.txt`.

### Inputs

An input cell says what has to be in the cube. First **what**, then, after commas, words that narrow
it down.

| Cell | Means |
|---|---|
| `r10` | a Thul Rune (an item code) |
| `"r10,qty=3"` | three Thul Runes |
| `shld` | any shield (an item type code) |
| `"shld,rar"` | a rare shield |
| `"weap,eth,sock=4"` | an ethereal weapon with four sockets |
| `any` | any item |

`numinputs` is the number of items in the cube in all: `"r10,qty=3"` and `gcy` make 4.

### Outputs

An output cell says what comes out. A recipe can have three: `output`, `output b`, `output c`.

| Cell | Means |
|---|---|
| `r11` | an Amn Rune |
| `"rin,mag,pre=372"` | a magic ring with magic prefix number 372 |
| `useitem` | the very item you put in as input 1, kept and changed |
| `"usetype,mag"` | a new magic item of the same kind as input 1 |

### Properties (mods)

Each output can be given up to five properties: `mod 1` to `mod 5` for `output`, `b mod 1` to
`b mod 5` for `output b`, `c mod 1` to `c mod 5` for `output c`. Each has five cells:

| Cell | Means |
|---|---|
| `mod 1` | the property's code (`res-cold`) |
| `mod 1 chance` | percent chance it is given; blank or 0 = always |
| `mod 1 param` | the property's parameter, when it needs one |
| `mod 1 min`, `mod 1 max` | the amount: from min to max; the same number in both = exactly that |

### Item level of the output

| Cell | Means |
|---|---|
| `lvl` | a fixed item level. When filled in, the other two are ignored. |
| `plvl` | percent of the player's level |
| `ilvl` | percent of input 1's item level |

`b lvl`, `b plvl`, `b ilvl` and `c lvl`, `c plvl`, `c ilvl` are the same for outputs b and c.

### Conditions

| Cell | Means |
|---|---|
| `enabled` | 1 = the cube uses the recipe, 0 = switched off |
| `min diff` | 0 = every difficulty, 1 = Nightmare and Hell, 2 = Hell only |
| `version` | 0 = also classic characters, 100 = expansion characters only |
| `class` | a class code (`ama`, `sor`, `nec`, `pal`, `bar`, `dru`, `ass`, `war`): only that class |
| `op`, `param`, `value` | one extra condition. `op` is a number that says which kind; the legend's Recipe columns section lists all of them. |

Example of `op`: `op` 18, `param` 370, `value` 0 means "stat number 370 of the item put in must be 0".

---

## 10. The files

Everything of the plugin is in one folder. `<mod>` is the name of the mod you play (`D2RMM` for
D2RMM).

```
Diablo II Resurrected/d2rloader/live-cube-recipes/<mod>/
```

| File | What it is |
|---|---|
| `recipes.txt` | **The cube's recipe list** (section 2). Written by the recipe book when you save. |
| `deleted.txt` | The defaults you removed. Written by the recipe book. |
| `templates/` | Your templates, one file each. |
| `all-recipes.txt` | Every recipe the cube has right now. Written by the plugin; changing it does nothing. |
| `legend.txt` | The legend as a text file. Written by the plugin at every start. |
| `README.txt` | A short reminder of this guide. |
| `recipes-classic-lod.txt`, `deleted-classic-lod.txt`, `all-recipes-classic-lod.txt`, `legend-classic-lod.txt` | The same files for classic and Lord of Destruction characters. |

The recipe book and its editor work on the Reign of the Warlock files.

---

## 11. Editing recipes.txt by hand

You do not have to: everything in this section can be done in the recipe book. But `recipes.txt`
is an ordinary tab-separated table with the columns of `cubemain.txt`, and you can edit it with the
program you use for the game's `.txt` tables.

One line is one recipe. The cube tries them from the top and takes the first that fits.

| You want to | Do |
|---|---|
| add a recipe | add a line |
| change a recipe | change its line |
| switch a recipe off | set its `enabled` to 0 |
| remove a recipe | delete its line |

Keep the `*complete` column in the first line: it says that the file is the whole list.

An empty file (only the first line) means "the cube has the defaults". The simplest way to get a
full file to edit is to open the recipe book, change anything, and save.

A recipe that you delete by hand is not noted in `deleted.txt`, so **Search for new recipes** will
offer it again. Remove recipes in the recipe book if you want them to stay away.

**After saving the file, type `recipes` in the game's console** (section 12). The file is read then,
and once at every start of the game. Nothing watches it in between.

---

## 12. Keys and console commands

| Key | Does |
|---|---|
| Ctrl+B | opens and closes the recipe book |
| Ctrl+L | opens and closes the legend |

Both keys can be changed in the game's key settings, under "Live Cube Recipes".

Console commands (the console opens with Ctrl + `):

| Command | Does |
|---|---|
| `recipebook` | opens and closes the recipe book |
| `recipelegend` | opens and closes the legend |
| `recipes` | reads `recipes.txt` again and has the cube use it (after editing the file by hand) |

---

## 13. Settings

`Diablo II Resurrected/d2rloader/config/live-cube-recipes.toml`. Close the game before changing it.

| Setting | Default | Means |
|---|---|---|
| `enabled` | `true` | `false` = the plugin does nothing |
| `console` | `true` | a line in the console when the list changes, and for every line the game could not read |

---

## 14. Good to know

- **Any item of your mod can be used**, including items that other plugins add. The plugin has no
  item list of its own; the game itself reads your cells.
- **After a mod update, press Search for new recipes.** Your list is your own; recipes the mod
  added do not appear in it by themselves. Recipes the mod *changed* keep the version in your list.
- **An item added to the mod while the game runs** is known to the game only after the next start.
- **A line with a mistake is left out**, with a message naming the line and the cell. Your other
  recipes are not affected.
- **A misspelt word after a comma** in an input or output (`qty=`, `eth`, `sock=` ...) is ignored by
  the game without a message, exactly as in `cubemain.txt`. Check such recipes in the cube.
- **Order matters.** The cube takes the first recipe that fits. New recipes go to the front of the
  list; recipes added by a search or a restore go to the end.
- **A recipe gives at most five properties to each output.** That is the game's limit.
- **Lists are kept per mod**, because item codes differ from mod to mod.
- **In a multiplayer (TCP/IP) game the host's recipes count.**
- **A D2RMM Install does not touch your files.**
- A recipe of the game that no recipe file has text for (rare; other plugins could make one) cannot
  be shown or edited. The cube keeps it all the same.
- Every scroll step closes and reopens the panel very quickly; you may see a short flicker.
- The buttons use the game's own button picture, read from your game (or your mod, if it has its
  own) at start and resized. If it cannot be read, buttons are shown as words in brackets,
  `[ Save ]`, and work the same; the log line starting `buttons:` says why.

---

## 15. If something goes wrong

Look first at the plugin's log:

```
Diablo II Resurrected/d2rloader/logs/live-cube-recipes.log
```

| What you see | What it means |
|---|---|
| Ctrl+B does nothing | Type `recipebook` in the console; if the panel cannot be made it says why. The log line starting `panels:` has the details. |
| "the game does not know ..." | An item code, item type or name in that cell does not exist in the mod. Look it up in the legend. |
| "... is not a property code" | The code in a `mod` cell does not exist. The legend's Recipe codes section lists the property codes. |
| A recipe is in the book but the cube does not use it | Is it switched off (`(not enabled)`)? Does `numinputs` equal the number of items? Is there an `op` condition? Is another recipe above it that fits the same items? |
| A recipe the mod has is missing from the cube | Your list does not have it. Press **Search for new recipes**, or **Restore deleted defaults** if you removed it. |
| You want the game's recipes back as they were | Save the list as a template if you want to keep it; then remove every line but the first from `recipes.txt` and type `recipes`. A list without recipes always means the defaults. |
| "recipes.txt could not be written" | Another program has the file open. Close it and save again; your changes are still in the book. |
| "something else replaced the game's recipe list" | Another plugin changed the list after the game started. Start the game again. |
| The log says the plugin is off because "this is another build of the game" | The game was updated. The plugin has to be updated for the new build. |
| Letters you type set off hotkeys while a panel is open | The plugin could not take the keyboard; the log says so. Use the arrows and the console instead. |
