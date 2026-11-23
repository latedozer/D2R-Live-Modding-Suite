# Mod Book - version 0.7.0

`d2rl-mod-book.dll`, version 0.7.0. A D2RLoader plugin for Diablo II: Resurrected, made for game
build 3.4.95041 and D2RLoader 1.3.1.

## What it does

Mod Book gives the game two side panels of its own kind: the **book** on the left and the
**legend** on the right. Other plugins of the suite put their pages into these panels as **tabs**.
Mod Book has no recipes or properties of its own. It draws the panels, the tab buttons, the
buttons and scroll bars, and it takes the keyboard while a panel is open, so that typing a letter
does not set off the game's hotkey for it.

It brings one tab of its own: **Property/Stat Terms**, in the **General** group on the right. It
explains the words the other tabs use: stat, property, func, val, set, op, param, signed and more.

## With and without the rest of the suite

- **Alone**, Mod Book opens its panels, but the only tab in them is Property/Stat Terms.
- **With Live Cube Recipes**: the recipe book (tab "Cube Recipes", left) and the recipe legend
  ("Recipe Words" in the "Cube Recipes" group, "Recipe Terms" in the "General" group, right).
- **With Live Properties**: "Properties" (left), and "Stats" and "Funcs" in the "Properties"
  group (right).
- The other plugins work without Mod Book, but then they have no panels: only their files and
  console commands.

## Installing

1. Close the game.
2. Copy `d2rl-mod-book.dll` into `Diablo II Resurrected/d2rloader/plugins/`.
3. Start the game through D2RLoader.

To remove it, delete the DLL. Your tab order file stays.

## Keys

| Key | Does |
|---|---|
| Ctrl+B | opens and closes the book (left side) |
| Ctrl+L | opens and closes the legend (right side) |

Both can be changed in the game's key settings, under "Mod Book".

While a panel is open:

| Key | Does |
|---|---|
| letters, digits, space | typed into the tab (a search, or a cell you are editing) |
| Backspace / Delete | erase a letter / empty the field |
| Up / Down, mouse wheel | scroll |
| Page Up / Page Down | a screenful |
| Home / End | to the start / end |
| Tab / Shift+Tab, Enter | next / previous cell, confirm |
| Ctrl+S, Ctrl+N, Ctrl+Z, Ctrl+Y, Ctrl+C, Ctrl+V | passed to the tab (save, new, undo, redo, copy, paste) |
| Escape | back one screen, or close |

Other Ctrl and Alt keys and the F keys stay the game's. With both panels open, keys and the wheel
go to the panel the cursor is over.

## Tabs and groups

- Click a tab's button to show it. On the right side, tabs sit in groups: a row of group buttons
  ("General" first), and under it the tabs of the group shown.
- **Moving tabs**: hold a tab's button and drag it:
  - onto another tab's button: it goes to that place (and that tab's group);
  - onto a group's button: it goes to the end of that group;
  - out of the panel, toward the other side of the screen: it goes to the other side.
- The order you make is kept in `d2rloader/live-suite/mod-book/tab-order.txt` and used at every start.
- `modbook tabs reset` in the console deletes that file. From the next start every tab is back
  where its plugin puts it.
- A side holds up to 9 tabs.

## Scroll bars

A thin bar on the right of a list shows where you are. Click on it to jump there, or hold the mark
and drag. Mod Book learns where the panel is on your screen from your first click on a tab button
or on the bar. Before that first click it starts from where the panel usually is in a 16:9 window, so dragging the bar works at once there; in a window of another shape click once first.

## Console commands

The console opens with Ctrl + `.

| Command | Does |
|---|---|
| `modbook` | opens and closes the book (left) |
| `modbook right` | opens and closes the legend (right) |
| `modbook tabs reset` | the tabs back where the plugins put them (from the next start) |

## Files

| File | What it is |
|---|---|
| `d2rloader/live-suite/mod-book/tab-order.txt` | your order of the tabs, written when you drag a tab |
| `d2rloader/config/mod-book.toml` | settings file; Mod Book has no settings |
| `d2rloader/logs/mod-book.log` | the plugin's log |

The button and scroll pictures are the game's own. They are read (never written) from your mod's
files or the game's data at start.

## Good to know

- A panel can only be opened in a game, not in the menus.
- Every page change closes one panel and opens the next very quickly. You may see a short flicker.
- D2RLoader allows a plugin 64 panels in all; Mod Book uses 32 on each side.


## License

Free software: you may use, copy, change and share it, under the GNU General Public License
version 3 (`LICENSE.txt`). Anyone who passes it on, changed or not, must pass on the source and the
same freedoms. Copyright (C) 2026 Latedozer. Other people's parts: `THIRD_PARTY_NOTICES.md`.
