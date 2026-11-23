# Soft Modding suite

Plugins for Diablo II: Resurrected that let you change the game's data **while you play**: cube
recipes, item properties, stats and magic affixes. No restart, no reinstalling your mod. When you
are happy with the changes, Live Export writes them out as a mod.

They are D2RLoader plugins, made for game build 3.4.95041 and D2RLoader 1.3.1.

## The five plugins

| DLL | Version | What it does |
|---|---|---|
| `d2rl-mod-book.dll` | 0.7.0 | **Mod Book**: two side panels with tabs that the other plugins put their pages into. Ctrl+B opens the left side, Ctrl+L the right side. |
| `d2rl-live-cube-recipes.dll` | 0.15.0 | **Live Cube Recipes**: your own Horadric Cube recipe list. Add, change, switch off and remove recipes in game. |
| `d2rl-live-properties.dll` | 0.7.0 | **Live Properties**: see and change every item property, stat and magic prefix / suffix of the game, and make new stats and affixes. |
| `d2rl-save-scan.dll` | 0.2.0 | **Save Scan**: reads every saved item on request, so that a change that would break a saved item can be refused. |
| `d2rl-live-export.dll` | 0.1.0 | **Live Export** (new): writes your mod with all these changes in it, as a whole mod folder or as a D2RMM mod. |

Each has its own README in this folder.

## How they fit together

- **Mod Book** is the shared screen. It has no content of its own except one tab, "Property/Stat
  Terms", that explains the words the other tabs use.
  - Left side (Ctrl+B): **Cube Recipes**, **Properties**, **Affixes**, **Export**.
  - Right side (Ctrl+L), in groups: **General** (Recipe Terms, Property/Stat Terms),
    **Cube Recipes** (Recipe Words), **Properties** (Stats, Funcs).
  - Tabs can be dragged by their buttons: onto another tab, onto a group, or over to the other
    side. Your order is kept in `d2rloader/live-suite/mod-book/tab-order.txt`. Type `modbook tabs reset` in
    the console to put every tab back where its plugin puts it (from the next start).
- **Live Cube Recipes** works on its own, through its file and the `recipes` console command.
  With Mod Book it has the in-game recipe book and legend.
- **Live Properties** puts its tabs into Mod Book, and asks Save Scan before it changes how a stat
  is saved. Without Mod Book it has no screen (its console command `prop` only opens tabs);
  without Save Scan your saved characters and stash are not checked before such a change.
- **Save Scan** does nothing until another plugin, or you in the console, asks it.
- **Live Export** reads what the other plugins keep (your recipe list, your property, stat and
  affix edits) and writes it out only when you ask (console `export`, or its tab).

## Which DLLs to install

| You want | Install |
|---|---|
| your own cube recipes, by file | Live Cube Recipes |
| your own cube recipes, in game | Live Cube Recipes + Mod Book |
| properties, stats and affixes | Live Properties + Mod Book + Save Scan |
| your changes as a mod | Live Export (+ Mod Book for its tab) |
| everything | all five |

To install: close the game, copy the DLLs into `Diablo II Resurrected/d2rloader/plugins/`, start
the game through D2RLoader. The order does not matter; the plugins find each other when the game
starts. To remove one, delete its DLL; its files stay.

## Compatibility

- **Exalted Items** is supported. Exalted Items keeps its own adjusted copies of the game's tables
  (stats, properties, cube recipes and more). Live Properties works on what the game read, those
  copies included. A **new stat** is made in one of the empty stat rows Exalted Items leaves in its
  stat table, so making stats needs Exalted Items; everything else works without it.
- A mod exported **with** Exalted Items' copies already holds Exalted Items' rows. Keep the Exalted
  Items plugin **on**: version 1.1.0 or later sees that and uses the exported tables as they are,
  adding only its game hooks (an older version would add its rows a second time). The "plain"
  export leaves Exalted Items' table copies out.
- Live Cube Recipes reads the mod's cube recipes and the copies other plugins keep, so recipes from
  your mod and from Exalted Items both show up.
- If the game is updated to another build, the plugins that depend on it switch themselves off
  (or parts of them) and say so in their logs, rather than guess.

## Your mod's files are never written

None of these plugins writes into your mod's folder (for D2RMM: the `D2RMM.mpq` folder). Each one
keeps its own files under `Diablo II Resurrected/d2rloader/live-suite/` (folders of older versions,
`d2rloader/<plugin>/`, are moved there at the first start):

| Folder | Plugin |
|---|---|
| `d2rloader/live-suite/live-cube-recipes/<mod>/` | your recipe list, templates, all-recipes and legend files |
| `d2rloader/live-suite/live-properties/<mod>/` | your property, stat and affix edits |
| `d2rloader/live-suite/save-scan/<mod>/` | `items.txt`, only when you ask for a dump |
| `d2rloader/live-suite/mod-book/` | `tab-order.txt` |
| `d2rloader/live-suite/mod-export/` | the mods Live Export writes |
| `d2rloader/config/` | each plugin's settings file |
| `d2rloader/logs/` | each plugin's log; look here first if something does not work |

A D2RMM Install does not touch these files.

## One warning

A character that holds an item with a stat made by Live Properties **cannot be loaded without the
Live Properties plugin** (or a mod exported by Live Export, which has the stat in its tables). Take
such items off before you remove it.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
