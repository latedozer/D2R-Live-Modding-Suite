<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE. -->
# Live Strings

`d2rl-live-strings.dll`: D2RLoader plugin of the Live Suite. It makes text keys while the game runs, so a stat made during play can show a line on its item tooltip with no restart.

How it works: every add or edit writes one new string file, `data/local/lng/strings/d2rloader/live-strings/add-<n>.json`. The plugin registers it with the loader's resource service, and the loader rebuilds its string tables at once. Every number already given out stays the same. A table cell such as a stat's `descstrpos` then names the key (`live-strings:<name>_r<revision>`). Live Tables' `setCells` writes that cell; Live Strings writes no table bytes.

Rules it keeps:
- **Nothing is ever unregistered.** Unregistering a file whose keys hold numbers makes the loader's rebuild fail, and that empties all game text.
- **A text edit is a new revision key** (`<name>_r2`, `_r3` …). Older revisions stay defined with their text.

## Build, test, install
- `./build.sh`: needs mingw-w64 and `../PluginSDK-master/include`.
- `./test/run_tests.sh` checks:
  - the core: names, revisions, refusals, the string files, the kept file read back at load;
  - that every file written is strict JSON;
  - the mod's 376 item-modifier texts in all 13 languages, through the writer and back;
  - D2RCore's stamp and the bytes `poolUse` reads;
  - the GPL headers.
- `./install.sh`: only when the user says "install". It refuses while the game is running.

## API (`live_strings_api.h`)
`livestrings::Find()` returns the API, or null when the plugin is not installed.

| Call | What it does |
| --- | --- |
| `add(items, n, done, user)` | Adds new names, all in one file (one rebuild). |
| `setText(items, n, done, user)` | Gives each name a new revision with the new text. |
| `number(name)` | The latest revision's two-byte number. |
| `key(name)` | The latest revision's full key. |
| `text(name, language)` | The latest text in one language. |
| `list(cb)` | Every revision, for Live Export. |
| `poolUse` | How many two-byte numbers are in use. |
| `status` | On, or why it is off. |
| `addListener` / `removeListener` | Calls you after each add or edit. |

`done()` runs on the UI thread with each key and its number, plus `hitchMs`, the loader's rebuild time. Put every key of one action in one call.

Languages: all 13 of the game's files (enUS zhTW deDE esES frFR itIT koKR plPL esMX jaJP ptBR ruRU zhCN). A language left out gets the enUS text, because D2RCore does not fall back to English.

## Files
- `d2rloader/live-suite/live-strings/<mod>/strings.json` holds every revision of every name. It is registered as `kept.json` while the plugin loads, so the kept texts are there before the game reads its tables.
- If the file can't be read, it is left as it is and nothing is added that start.

## Console
- `strings`: names, revisions and pool use.
- `strings add|set <name> <enUS text>`, `strings get <name>`, `strings list`.
- `strings probe`, then `strings probe swap`: tests whether a plugin can override its own key and swap that override in place. If it can, an edit could keep one key and one number. Each step's result goes to the log.

## Limits
- D2RCore's two-byte pool holds 65535 numbers in all, the game's own ids included. Each revision a table names uses one.
- Each add or edit costs one rebuild of the loader's string tables (a hitch, logged in ms).
- Never run in game yet.

## Changes
- 0.1.0 (2026-11-25): first build. add / setText / list, kept file, override probe.
