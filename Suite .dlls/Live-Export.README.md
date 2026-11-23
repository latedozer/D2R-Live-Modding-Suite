# Live Export

`d2rl-live-export.dll`, version 0.1.0 (**new, not yet tried in the game**). A D2RLoader plugin for
Diablo II: Resurrected, made for game build 3.4.95041 and D2RLoader 1.3.1.

## What it does

Writes the mod you are playing **with all the changes you made in the Live Suite in it**: your cube
recipe list (Live Cube Recipes) and your property, stat and affix edits (Live Properties). It does
nothing until you ask, and writes only into `d2rloader/live-suite/mod-export/`. Your mod's own
folder is only read.

Two kinds of export:

- **A whole mod** (`.mpq` folder): a copy of every file of your mod, with the changed tables written
  in. Tables your mod does not have (for example `magicprefix.txt`) are taken from the game's own
  data. Names of affixes you made are added to the mod's strings. The mod keeps your save folder,
  so your characters are there.
  - By default the tables Exalted Items keeps adjusted copies of are taken from those copies (what
    the game really reads). **Play such an export with the Exalted Items plugin switched off**, or
    its rows would be added a second time.
  - The **plain** export leaves Exalted Items' copies out; the Exalted Items plugin then works on it
    as on any mod.
- **A D2RMM mod** (`mod.json` + `mod.ts`): the same changes, made by D2RMM on top of your other
  mods. It changes rows in place - properties found by code, stats and affixes by number (checked by
  name), recipes taken out and put in one by one - so the other mods' changes stay. A stat you made
  keeps its number: the mod fills the stat table with empty rows (`live_unused_<n>`) up to it, and
  **stops with an error** if another mod already has a stat at that number, because saved items
  with your stat would then read wrong.

Every export has an `export-report.txt` that lists what is in it and what was left out.

## What it needs

- Nothing to run. **Mod Book** for its tab; the console works without it.
- It exports what Live Cube Recipes and Live Properties keep; without them there is nothing to export.

## How to use it

In the Mod Book (Ctrl+B), tab **Export**: type a name, then a button. Or in the console (Ctrl + `):

| Command | Does |
|---|---|
| `export` | what it can do |
| `export mpq [name]` | the whole mod, with Exalted Items' table copies |
| `export mpq [name] plain` | the whole mod, without them |
| `export d2rmm [name]` | the D2RMM mod |

The name defaults to `<your mod>-live`. A name has letters, digits, spaces, `-` and `_`. An earlier
export of the same name is replaced.

## Where it goes

| Folder | What to do with it |
|---|---|
| `d2rloader/live-suite/mod-export/<name>/` | copy `<name>` into the game's `mods` folder, start the game with `-mod <name>` |
| `d2rloader/live-suite/mod-export/<name>-d2rmm/` | copy it into D2RMM's `mods` folder, put it after the mods it was made with, install |

## Known limits

- Never tried in the game yet. Its file logic is tested on the game's own tables, and the D2RMM mod
  it writes was run against them with a stand-in for D2RMM.
- An affix's class cells (classspecific, class, classlevelreq) are not exported: the game keeps
  them as numbers, the tables as class codes. The report says so when you changed one.
- A stat's text cell that holds a string number no key is known for keeps the table's own text; the
  report says so.
- The whole-mod export copies every file of the mod (D2RMM: about 360 MB); it takes a moment.

Settings: none. Log: `d2rloader/logs/live-export.log`.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
