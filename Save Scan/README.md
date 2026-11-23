# Save Scan

A D2RLoader plugin that reads every saved item - all characters (`.d2s`) and the shared stash
(`.d2i`) of the mod's save folder - and says which items hold a stat. It exists so that other
plugins can ask, before they change how a stat is saved, whether an item somewhere would stop
being readable.

It does nothing until asked. No hooks, no timers. Saves are only read.

- Other plugins: `save_scan_api.h` (`savescan::Find()`, `scan`, `dump`, `savescan::Safe`).
- Console: `savescan` (reads and counts), `savescan <stat number> [...]`, `savescan dump`
  (writes `d2rloader\save-scan\<mod>\items.txt`: every item with its stats, then every stat in
  use with the properties that name it).

Build `./build.sh`, test `./test/run_tests.sh`, install `./install.sh` (refuses while the game runs).

## What a "read" file means

A file counts as read only when every item list ends exactly on the marker or section end that
must follow it. Anything else - an unknown item code, a stat without save bits in the table, a
list that runs past its end - makes the file UNREADABLE, and `savescan::Safe` is then false.

## The item layout (save version 105), and how sure each part is

Worked out from 15 real files (1141 items) in November 2026; the layout is in `save_core.h`.

Confirmed by those files (every list landed on its end marker):
- the header fields, the Huffman item code, items in sockets;
- simple items: 2 bits for quest items (`questdiffcheck`), a flag + 128-bit id, a flag + 8-bit quantity;
- other items: quality data (low, normal, superior, magic, set, rare, unique, crafted; 12 bits
  for a normal-quality charm), runeword word, tome 5 bits, flag + 128-bit id, defense,
  durability, classic 9-bit quantity, socket count, set list mask, the stat lists,
  the closing flag + 8-bit quantity;
- characters: items, corpse, mercenary, golem, the closing `lf` section with count 0;
- stash: tabs, the materials section.

Not confirmed (no such item or case in the files):
- an ear; a personalized item's name (read as 8-bit letters up to a 0);
- the one bit after the 128-bit id of a non-simple item: read and ignored (its meaning is not known);
- item flag 28 (set on 2 items, a unique and a set item; the game's code ties it to its record of
  found items): read as 55 more bits at the very end. Both samples fit; the order of that block
  and the quantity flag before it is a guess;
- an item without the 128-bit id (flag 0);
- the `lf` section with a count other than 0, and the tail of the stash's record section
  (type 2): not looked into for items. A character whose `lf` count is not 0 is reported unreadable;
- classic and LoD characters (tried with the data version 2 and 1 stat tables when version 3 fails);
- the stats that are saved together (17; 48, 50, 52; 54, 57) are the game's usual ones, taken as given;
- in the game: the row places of save bits (+0x15), save add (+0x18) and save param bits (+0x20)
  are checked at each scan against four stats every table has (97, 188, 204, 31); the plugin
  DLL itself has never run in the game.
