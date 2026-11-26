// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// Live Tables (d2rl-live-tables.dll): grows the game's ItemStatCost and Properties tables while the
// game runs, in the game's own memory, with the game's own row rules. A grown row is usable at once
// (an item can carry the stat, an affix / recipe can name the property code) and is laid into the
// table's text again every time the game reads it, so it is there at the next start too.
// This header is all a plugin needs; nothing is linked.
//
//   const livetables::Api* lt = livetables::Find();     // nullptr: Live Tables is not installed
//   if (lt) lt->grow(livetables::Table::ItemStatCost, 0, "Stat=my_stat\tsaved=1\tsave bits=13", OnDone, me);
//
// THREADS. Call check / count / limit / find / revision from the game thread or the UI thread
// (they read the game's tables and name tables, which only change in a growth's job, in a way a
// UI-thread reader is safe with). grow and
// the listener calls may be made from any thread.
//
// WHERE A GROWTH RUNS. grow() queues the row. The growth runs in a job on the game thread (inside
// the authoritative update: no item creation and no loot roll run meanwhile); at the main menu (no
// game thread) in a UI job. NOBODY WAITS FOR ANOTHER THREAD, so a growth cannot hold up a frame.
// When the loader's UI thread (D2RCore records it at 0x7df080) is another thread than the game job's,
// the UI may run during the growth, so everything is done in a way a UI reader is safe with: rows
// in new memory published begin-then-count (the old buffer stays), the name appended at the tail of
// its bucket chain (a growth that would make the game rebuild the bucket array is refused then),
// D2RCore's name list swapped under D2RCore's lock; the description order (D2RCore frees its old
// array) is redone in a UI job afterwards, then done() runs. If the game job does not run within
// 3 s, done() says "busy, try again". The log says which threads it saw.
// WHAT A GROWTH DOES (in every data version it is asked for):
//   1. new memory from the game's allocator for count + 1 rows; the OLD ROWS ARE COPIED AS THEY ARE
//      (edits other plugins made in place, their op links included, are carried over);
//   2. the new row is compiled as the game's row compiler 0xa12770 would (pass 1: its name into the
//      game's name table with the game's own insert 0xa11e10; pass 2: every cell, names looked up with
//      0xa121f0, string keys with 0x308be0);
//   3. OLDER rows whose cell names the new name and is still unresolved (0xffff) get the new number,
//      as a load would have given them (a grown stat: in ItemStatCost and in Properties);
//   4. ItemStatCost: the new row and those older rows are linked where the load would have linked
//      them (op lists by stat number); nothing else is relinked. Then D2RCore's
//      BuildItemStatDescriptionOrder rebuilds the description order;
//   5. publish: begin, then count. The OLD BUFFER STAYS VALID AND UNCHANGED UNTIL THE NEXT TABLE
//      LOAD (Live Tables frees it then): a caller may still read it after the change callback;
//   6. the change listeners are called (see Change), in the growth's job;
//   7. the row is kept in d2rloader\live-suite\live-tables\<mod>\grown-rows.txt; done() runs later on
//      the UI thread.
// Numbers: the row gets the number the game's name table gives it, which Live Tables checks is the
// row count (no spare rows, no gaps). Id maps that keep a number across a changed mod come with
// package 1.2b. Data versions 1 and 2 read the same text (excel\base): growing one grows both; when
// they are one data tables object (as in game 2026-11-25) a Change comes for each of 1 and 2.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace livetables {

constexpr uint32_t kApiVersion = 5; // 2 (0.2.0): disable / enable / remove / isDisabled; 3 (0.3.0): rename, remove of Properties, Change kind Rename; 4 (0.3.2): status, fixMoved, moved, setCells, Change kinds Alarm / CellEdit; 5 (0.3.2): list

enum class Table : int32_t { ItemStatCost = 0, Properties = 1 };

// done(): exactly once per grow() / disable() / enable() / remove() call, on the UI thread.
struct GrowDone {
	uint32_t    size;        // sizeof(GrowDone)
	int32_t     ok;          // 1 = grown in every data version asked for, 0 = nothing changed anywhere
	int32_t     numbers[4];  // by data version (1 classic, 2 LoD, 3 RotW; [0] unused): the row's number, -1 = not done there
	uint32_t    relinked;    // older rows that now find the new name (all data versions)
	const char* why;         // refusal, or notes on success (may be ""); valid during the call only
};
using DoneFn = void(__cdecl*)(void* user, const GrowDone* done) noexcept;

// A change of a table. kind Grow: called in the growth's job (the game thread; the UI thread at the
// menu), right after the new buffer is published; the UI thread may be running (see above). kind Load: the game has read its tables again (called from
// the DataTablesLoaded event); all earlier rows pointers are gone then (oldRows = nullptr).
// A listener must be short, must not take a lock (record the change and apply it at your next own
// entry), must not call grow / addListener / removeListener (check / count / limit / find / revision
// are fine). Rows keep their numbers: row n of
// oldRows is row n of newRows for n < oldCount.
// Disable / Enable: row `number` changed in place (oldRows == newRows, counts the same).
// Shrink: the LAST row went (newCount = oldCount - 1; rows stay where they are; the old last row's
// bytes stay readable until the next table load). Remove: a middle row went: newRows is new memory
// without it, every row above `number` is one lower (map). For Shrink and Remove, map[old] is the
// new number of row `old` (-1 for the removed one), mapSize = oldCount; it stays valid and
// unchanged until the next table load.
// Rename: row `number` has a new name (oldName / newName); no row byte changed.
// Alarm: something needs the user's eyes NOW (status() has the text): show it on screen.
// CellEdit: cells of row `number` changed in place (cells = the "column=value" text that was set).
enum class ChangeKind : int32_t { Grow = 0, Load = 1, Disable = 2, Enable = 3, Shrink = 4, Remove = 5, Rename = 6, Alarm = 7, CellEdit = 8 };
struct Change {
	uint32_t       size;        // sizeof(Change)
	ChangeKind     kind;
	Table          table;
	int32_t        dataVersion; // 1 classic, 2 LoD, 3 RotW
	const uint8_t* oldRows;     // valid until the next table load (Grow); nullptr (Load)
	const uint8_t* newRows;
	uint64_t       oldCount, newCount;
	uint64_t       rowSize;     // 0x144 ItemStatCost, 0x30 Properties
	uint64_t       revision;    // revision() after this change
	// version 2 (check size >= offsetof(Change, mapSize) + 8)
	uint64_t       number;      // the row a Disable / Enable / Shrink / Remove is about (Grow: the new row)
	const int32_t* map;         // Shrink / Remove: old -> new (see ChangeKind), else nullptr
	uint64_t       mapSize;
	// version 3 (check size >= offsetof(Change, newName) + 8): Rename only, valid during the call
	const char*    oldName;
	const char*    newName;
	// version 4: CellEdit only, valid during the call
	const char*    cells;
};
using ChangeFn = void(__cdecl*)(void* user, const Change* change) noexcept;

// A remove veto (version 3): a plugin whose own files name rows by number or name (recipes.txt ...)
// that Live Tables' read hook never sees. remove() asks every registered veto first, on the thread
// that calls remove(), before anything is queued; Live Tables holds none of its locks then, so a veto
// may take its own. Return 1 = it may go; 0 = refused, the reason in `why` (shown in done()). A
// middle row moves every row after it one lower (`number` .. count - 1): refuse if your file holds
// any of them. A query, not a lock: decide from what you hold now. Never call remove() from it.
using CanRemoveFn = int(__cdecl*)(void* user, Table table, int dataVersion, int number, char* why, size_t whySize) noexcept;

struct MovedRow {        // version 4
	char    name[48];
	int32_t was, now;    // the number when made, the number this start
	int32_t dataVersion; // 3, or 1 (data versions 1 and 2 read one text)
};

struct KeptRowInfo {     // version 5, valid during the call
	uint32_t    size;       // sizeof(KeptRowInfo)
	int32_t     number;     // its number in the asked data version (as kept in grown-rows.txt; -1 = not known there)
	const char* name;
	const char* cells;      // tab-separated "column=value", as kept (text: names, string keys)
	const char* dataVersions; // "3", "12", "123"
	int32_t     disabled;   // 1 = in disabled-rows.txt for that data version
};
using KeptFn = void(__cdecl*)(void* user, Table table, const KeptRowInfo* row) noexcept;

struct Api {
	uint32_t size, version;
	// Would grow() take this row in `dataVersion` (0 = every data version that has the table)?
	// 1 = yes; 0 = no, the reason in `why`. Changes nothing.
	// rowText: one row, either tab cells under the table's own header (the one the game read) or
	// tab-separated "column=value" cells (column names as in the header, any case, any order).
	int(__cdecl* check)(Table table, int dataVersion, const char* rowText, char* why, size_t whySize) noexcept;
	// Queues the row for the growth's job. Returns 1 (done will be called) or 0 (done is null: nothing queued).
	int(__cdecl* grow)(Table table, int dataVersion, const char* rowText, DoneFn done, void* user) noexcept;
	int(__cdecl* count)(Table table, int dataVersion) noexcept;  // the rows now; -1 = not read
	int(__cdecl* limit)(Table table, int dataVersion) noexcept;  // most rows the game takes: 511 stats with the
	                                                             // game's own save format (32768 with D2RCore's wide
	                                                             // format), 32767 properties; for "N of M used"
	int(__cdecl* find)(Table table, int dataVersion, const char* name) noexcept; // the game's name table: number or -1
	uint64_t(__cdecl* revision)() noexcept;                      // goes up with every change (Grow or Load)
	// Returns a handle > 0, or 0. Any thread, not from inside a callback.
	int(__cdecl* addListener)(ChangeFn callback, void* user) noexcept;
	void(__cdecl* removeListener)(int handle) noexcept;
	// ---- version 2 (ItemStatCost only; Properties are disabled by Live Properties itself)
	// Disable: the row keeps its number and how it is saved (saves keep loading; items keep the
	// value) but has no effect: these bytes change IN PLACE - op +0x50 = 0 and its own op links out
	// (the base row's list +0x5a, the targets' op lists +0xda, its flag bits 4-7 at +4),
	// descfunc +0x32 = 0, dgrp +0x3a = 0, itemevent1/2 +0x44/+0x46 = 0xffff; then the description
	// order is rebuilt. Links INTO it (other rows using it as op base / op stat) stay. A unit's totals
	// already made with its op stay until that unit is recalculated (item moved / re-equipped).
	// Kept in d2rloader\live-suite\live-tables\<mod>\disabled-rows.txt (by name) and done again
	// every time the game reads its tables. Enable puts those cells back and links it where a
	// load would. Any row may be disabled; which ones to offer is the caller's rule.
	int(__cdecl* disable)(Table table, int dataVersion, int number, DoneFn done, void* user) noexcept;
	int(__cdecl* enable)(Table table, int dataVersion, int number, DoneFn done, void* user) noexcept;
	int(__cdecl* isDisabled)(Table table, int dataVersion, int number) noexcept; // 1 / 0
	// Remove: only rows Live Tables grew. Refused while anything in the game's tables still names
	// it (another stat's op base / op stat / maxstat, a property's stat1-7, a table that read it
	// at load) - done() says which. The LAST row: in a game too (refused while a saved item or an
	// item of the loaded character holds it: "leave to the menu..."). A MIDDLE row: at the main menu
	// only ("leave to the menu to remove a middle row"); every row above it becomes one lower,
	// in the tables, in Live Tables' files and, through Save Scan's writer (fixSaves, run BEFORE
	// the table changes; if it fails nothing changes), in every save file. With save holders at the
	// menu the same writer drops the stat's entries from every saved item first.
	int(__cdecl* remove)(Table table, int dataVersion, int number, DoneFn done, void* user) noexcept;
	// ---- version 3
	// Rename a row Live Tables grew (ItemStatCost or Properties): its number stays; the game's name
	// table gets the new name for it (the old one leaves), D2RCore's stat-name list (stats) too, and
	// Live Tables' files and the kept rows that name it follow. Refused when the new name is taken or
	// a table read at load names the old one (it would lose it at the next start). Change kind Rename.
	// Properties: remove() now takes the LAST row Live Tables grew (items save stats, not properties,
	// so no save is touched); refused while an affix, cube recipe, unique, runeword or a table read at
	// load names its code. A middle property row is refused for now.
	int(__cdecl* rename)(Table table, int dataVersion, int number, const char* newName, DoneFn done, void* user) noexcept;
	int(__cdecl* addVeto)(CanRemoveFn canRemove, void* user) noexcept;  // handle > 0, or 0
	void(__cdecl* removeVeto)(int handle) noexcept;
	// ---- version 4
	// 1 = an alarm is up, its text in `text` (show it on screen; 0 = none). Today's alarm: a made stat
	// whose number moved at this start (a mod update below it, or an exported mod holding it at
	// another number): its saved items read as another stat. Every ItemStatCost change is refused
	// while it is up. Any thread.
	int(__cdecl* status)(char* text, size_t size) noexcept;
	// At the main menu: Save Scan renumbers the saves to the made stats' numbers of this start
	// (dated backup, read back, put back on any difference; refused for a save written after this
	// start, which cannot be told apart), then the alarm goes down (Change kind Alarm again,
	// status() 0). In a game: refused ("leave to the menu").
	int(__cdecl* fixMoved)(DoneFn done, void* user) noexcept;
	// The made rows whose number at this start differs from the one they had when made (stats: the
	// alarm; properties: their kept number followed, no save involved). Filled at the table read,
	// valid until the next one. dataVersion 0 = every one. Writes up to `room`, returns how many.
	// Any thread.
	int(__cdecl* moved)(Table table, int dataVersion, MovedRow* out, size_t room) noexcept;
	// Cells of a row Live Tables grew: tab-separated "column=value" (column names as in the table's
	// header, any case; an empty value empties the cell). The row is compiled again the game's way
	// (names looked up, string keys through D2RCore - a key it does not know is refused), its op links
	// and the description order follow, and the cells are kept in grown-rows.txt (dated copy first) as
	// TEXT, so the next start compiles them again. Live Tables' kept row is the one source of a made
	// row's cells. Not the row's own name (rename), not a disabled row (enable it first).
	// ItemStatCost cells that are part of how items are SAVED: save bits / save add / save param bits
	// go through Save Scan's resize first, at the main menu only (dated backup, read back, put back on
	// any difference; in a game: "leave to the menu"); signed, valshift, saved, 1.09-*, csv* are
	// refused when a save holds the stat. Change kind CellEdit.
	int(__cdecl* setCells)(Table table, int dataVersion, int number, const char* cells, DoneFn done, void* user) noexcept;
	// ---- version 5
	// The rows Live Tables keeps (grown-rows.txt as it is now, not game memory), of `table` in
	// `dataVersion` (0 = every data version), in file order: callback once per row, on the calling
	// thread, after Live Tables has let go of its lock (the callback may call Live Tables). Any
	// thread. Returns how many rows were given.
	int(__cdecl* list)(Table table, int dataVersion, KeptFn callback, void* user) noexcept;
};

inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-live-tables.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "LiveTablesGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= offsetof(Api, disable) ? api : nullptr; // check version >= 2 before the version 2 calls
}

} // namespace livetables

// A plugin that reads itemstatcost.txt / properties.txt itself through D2RCore's read routine
// (Live Properties up to 0.9.x) and leaves that to Live Tables from now on exports this symbol
// (any value); Live Tables only adds its rows when every such plugin in the plugins folder has it:
//   extern "C" __declspec(dllexport) const int LivePropsUsesLiveTables = 1;
