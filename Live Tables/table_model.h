// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// Live Tables core: how the game (image 6ab3782c, D2RLoader 1.3.1, D2RCore.dll of Sep 23) builds
// ItemStatCost and Properties, and how a table grows by one row while the game runs. Every routine
// is a port of one found in the game (address in the comment). The core works on rows in the game's
// layout and asks a Resolver for names and strings: the plugin's Resolver calls the game's own
// routines, the native test's Resolver is the model name table below. No Windows, no game process.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lt {

// ---- the game's name tables (object made by 0xa11fa0, 0x40 bytes) -------------------------
// +0x00 sorted vector {u32 code, i32 number} for 4-letter codes (kinds 10/12/14, not used here)
// +0x18 hash map: +0x18 bucket count (starts at 8), +0x20 buckets (one pointer each),
//       +0x28 element count, +0x30 max load factor (float 1.0)
// +0x38 next number, +0x3c the number of the empty name (-1 until "" is put in)
// node (0x38 bytes, game allocator, align 0x10): +0 next, +8 key string {ptr, size, cap|flag,
//       16 inline bytes}, +0x30 i32 number
struct Node {
	Node*       next = nullptr;
	std::string key;      // lower case, as the game keeps it
	int32_t     number = 0;
};

struct NameTable { // the model (the game's own is used in game)
	std::vector<Node*> buckets = std::vector<Node*>(8, nullptr);
	size_t  count       = 0;      // +0x28
	float   maxLoad     = 1.0f;   // +0x30
	int32_t next        = 0;      // +0x38
	int32_t emptyNumber = -1;     // +0x3c
	size_t  rehashes    = 0;
	std::vector<std::unique_ptr<Node>> owned;

	// 0xa11e10(table, text, keepExisting): lower-case (0xa25650), hash (0x122c750), emplace 0xa0f630:
	// walk the bucket chain, append a NEW node at its tail, then rebuild the buckets at twice the
	// count (0x1f7d70) when count / buckets > maxLoad. A name already there: keepExisting -> its
	// number, else a fresh number all the same.
	int32_t Insert(const std::string& text, bool keepExisting);
	int32_t Find(const std::string& text) const; // 0xa121f0 -> number or -1; "" -> emptyNumber
	bool InsertRehashes() const { return float(count + 1) / float(buckets.size()) > maxLoad; }
	size_t BucketOf(const std::string& lower) const;
	void Rehash(size_t want);
	// a removed row: its node out of its chain; numbers above it one lower; the counter one lower
	bool Remove(const std::string& text);
	// a renamed row: the old name's node out, the new name in with the same number (counter kept)
	bool Rename(const std::string& was, const std::string& now);
	void Renumber(int32_t removed);
};

std::string Lower(const std::string& s);   // 0xa25650: the table at 0x1d9c980 (A-Z only)
// 0x122c750: MurmurHash3 x64-128 with seed 0 (constants at 0x2888430); out[0] picks the bucket
void Murmur3(const void* data, size_t len, uint32_t seed, uint64_t out[2]);

// ---- column descriptors (0x20 bytes: name, u32 kind, u32 n, u64 offset, extra) --------------
// Read from the stack tables the loaders build: ItemStatCost 0x31e310, Properties 0x319fb0.
enum Kind : uint32_t {
	KDword = 2, KWord = 3, KByte = 4,
	KName  = 0x11,  // the row's own name: into the table's name table in a first pass over all rows
	KLookup = 0x16, // a name in a name table -> word, 0xffff when unknown
	KString = 0x18, // a string key -> u16 string number through 0x308be0 (unknown key -> 0x1506)
	KBit    = 0x1d, // bit n of the dword at offset, set when the number is not 0
};
enum NameTableId { StatNames = 0, PropNames = 1, Events = 2 }; // datatables +0x1270, +0x270, +0x1d8
struct Column {
	const char* name;
	Kind        kind;
	uint32_t    n;       // bit number for KBit
	uint32_t    offset;
	int         table;   // NameTableId for KName / KLookup
};
enum class TableId : int32_t { ItemStatCost = 0, Properties = 1 };
extern const std::vector<Column> kStatColumns;      // 52 columns, row 0x144
extern const std::vector<Column> kPropertyColumns;  // 30 columns, row 0x30
constexpr size_t kStatRow = 0x144, kPropRow = 0x30;
const std::vector<Column>& ColumnsOf(TableId t);
size_t RowSizeOf(TableId t);
int NamesOf(TableId t); // the name table a table's own names go into

// limits found in the game
constexpr size_t kMaxStatsGame   = 0x8000; // assert "statsTbl.size() <= MAX_STATS" (0x31ed31); D2RCore desc order refuses >= 0x8001
constexpr size_t kMaxStatsRetail = 511;    // ids 0..510: 9-bit ids, 0x1ff ends the list (reader 0x37b7d4, writer 0x37f186)
constexpr size_t kMaxProperties  = 32767;  // GetPropertyRecord 0x3d3210: id < 0x10000, then used as a signed word: rows 0..32766

// what the core asks the game (or the model) for
struct Resolver {
	std::function<int32_t(int names, const std::string& text)> find;   // 0xa121f0
	std::function<int32_t(int names, const std::string& text)> insert; // 0xa11e10(table, text, 1)
	std::function<uint16_t(const std::string& key)>            stringId; // 0x308be0
};

// ---- text ------------------------------------------------------------------------------
// The tokenizer 0xa138a0: first line = header, cells split by tab, a row whose first cell is
// "Expansion" is skipped (0xa1528c). The header is mapped to the columns case-insensitively (0xa101f0).
struct Sheet {
	std::vector<std::string> header;
	std::vector<std::vector<std::string>> rows;
};
Sheet ReadSheet(const std::string& text);
// A row given to grow(): either tab cells under `header`, or "column=value" cells (tab-separated,
// any order, column names as in the header, case-insensitive). -> the cells under `header`.
bool RowCells(const std::vector<std::string>& header, const std::string& rowText, std::vector<std::string>& cells, std::string& why);
// "column=value" cells of a row (only filled cells): the form Live Tables keeps rows in
std::string PairsOf(const std::vector<std::string>& header, const std::vector<std::string>& cells);
// the row's own name (the KName column) under `header`
std::string NameIn(const std::vector<std::string>& header, const std::vector<std::string>& cells, TableId t);

// ---- compile, as the game does it --------------------------------------------------------
// The row compiler 0xa12770: pass 1 = the KName cells of all rows into the name table (keep = 1),
// pass 2 = every other cell, row by row; columns the file lacks get the defaults of 0xa13624.
std::vector<uint8_t> Compile(const Sheet& sheet, TableId t, const Resolver& r);
// one row, both passes (pass 1 puts its name in: the caller checked the number it will get)
void CompileRow(const std::vector<std::string>& header, const std::vector<std::string>& cells, TableId t, uint8_t* row, const Resolver& r);

// ---- derived data of ItemStatCost, on rows in the game's layout ---------------------------
// 0x31edd1 (+0x5a..+0xd9 to 0xff) and the op linker 0x31ee83..0x31f09d for every row
void LinkStats(uint8_t* rows, size_t count);
// One row's turn of that loop. ordered = put its entries where the load would have them (by stat
// number, in the base row's list and in each target's op list) instead of at the first free place.
void LinkStat(uint8_t* rows, size_t count, size_t i, bool ordered);
// Takes row i's entries out of every list and clears what only its link set (flag bits 4-7 at +4).
void UnlinkStat(uint8_t* rows, size_t count, size_t i);
// D2RCore BuildItemStatDescriptionOrder (export 0x8112c0): rows with descfunc (+0x32) != 0,
// stable-sorted by descpriority (+0x30, signed word), ascending
std::vector<uint16_t> DescOrder(const uint8_t* rows, size_t count);

// ---- growing ---------------------------------------------------------------------------
// Checks a row against a table as it is now (nothing changed). `sheet` = the text the game read for
// this table (needed: its header, and its rows are the table's rows 0..count-1).
struct Check {
	bool        ok = false;
	std::string why;          // refusal
	std::string name;         // the row's own name
	std::vector<std::string> cells; // under sheet.header
	size_t      number = 0;   // the number it will get (= the row count)
};
Check CheckRow(TableId t, const Sheet& sheet, size_t count, int32_t nameNext, size_t limit, const std::string& rowText, const Resolver& r);
// Grows a table by the checked row: `next` (count + 1 rows) must hold the old rows (copied as they
// are: in-place edits of other plugins stay) and room for one more. Writes the new row (both
// passes: its name goes into the name table), then the OLDER rows that name the new name get that
// cell (as pass 2 at load would have given them; their numbers in `relinked`), then for ItemStatCost
// the new row and the relinked rows are linked where the load would have linked them.
// The caller publishes `next` (begin, then count) and redoes the desc order.
struct Grown {
	size_t number = 0;
	std::vector<size_t> relinked; // older rows that now find the new name
};
Grown GrowInto(TableId t, const Sheet& sheet, const Check& c, uint8_t* next, const Resolver& r);
// Older rows of `sheet` whose cell names `name` in name table `names` (only these become different)
std::vector<std::pair<size_t, const Column*>> RowsNaming(TableId t, const Sheet& sheet, int names, const std::string& name);
// Writes `number` into those cells of `rows` (in place, one word each) where the cell is still 0xffff
// (another plugin's edit is left alone) -> the rows changed. A grown stat: the caller runs this on the
// Properties rows too (with the Properties text).
std::vector<size_t> RelinkCells(TableId t, const Sheet& sheet, uint8_t* rows, size_t count, int names, const std::string& name, uint16_t number);

// ---- disable / remove (ItemStatCost) -----------------------------------------------------
// Disable: the row keeps its number and its save cells (save bits +0x15, save add +0x18, save
// param bits +0x20, csv bits), so every save keeps loading; it gets no effect: its own op links out
// (UnlinkStat), op +0x50 = 0, descfunc +0x32 = 0 and dgrp +0x3a = 0 (no tooltip line, after the
// description order is rebuilt), itemevent1/2 +0x44/+0x46 = 0xffff. Links INTO it (other rows
// whose op base / op stat it is) stay: they are those rows' own effects.
struct OffCells { uint8_t op = 0, descfunc = 0; uint16_t dgrp = 0, event1 = 0xffff, event2 = 0xffff; };
OffCells DisableStat(uint8_t* rows, size_t count, size_t i);          // in place -> the cells it had
void     EnableStat(uint8_t* rows, size_t count, size_t i, const OffCells& was); // cells back, links back where a load puts them
bool     IsInert(const uint8_t* row);                                  // op, descfunc, dgrp 0, events none
// What still names stat `n`: other ItemStatCost rows (op base / op stat / maxstat) and property rows
// (stat1-7) -> "row 392 op base", "property 303 stat1" ...
std::vector<std::string> StatNamedBy(const uint8_t* stats, size_t count, size_t n, const uint8_t* props, size_t propCount);
// Remove stat n: `out` (count - 1 rows) gets every row but n, numbers above n one lower in every
// stat word (the row's own +0, op base, op stat1-3, maxstat), all op links made again from the cells
// (exactly what a load of the text without the row gives).
void RemoveStatRows(const uint8_t* rows, size_t count, size_t n, uint8_t* out);
// Property rows' stat1-7 words above n one lower (in place) -> rows changed
size_t RenumberPropStats(uint8_t* props, size_t propCount, size_t n);

// ---- names in other tables' texts ------------------------------------------------------
// The rows (1 = the first row after the header) of a table's text that name `name`: a cell equal
// to it (skills' aurastat, states' stat, items' stat1-3 ...) or the name inside a cell, set off by
// quotes, commas, parentheses or a following '.' (calc formulas: stat('name'.accr)). Case is not
// minded; a longer name that starts with it (lt_c / lt_cx) is not it.
std::vector<size_t> NamedRows(const std::string& text, const std::string& name);

// ---- persistence: rows kept in Live Tables' file, added again when the game reads the table -----
struct KeptRow {
	TableId     table = TableId::ItemStatCost;
	std::string versions;       // data versions it was grown in, e.g. "3" or "12"
	size_t      number = 0;     // the number it had when made: in data version 3 (excel\), or in 1/2 when it was grown there only
	size_t      numberBase = SIZE_MAX; // the number in data versions 1/2 (excel\base\), when it differs or is known (nb=)
	std::vector<std::pair<std::string, std::string>> cells; // column = value
	std::string name;
};
std::vector<KeptRow> ReadKept(const std::string& file, std::vector<std::string>* problems);
std::string KeptLine(const KeptRow& k);
// The number a kept row had when made, in the text `which` (0 = excel\ for data version 3, 1 =
// excel\base\ for 1/2); SIZE_MAX = not known for that text (a line written before 0.3.1 for a row
// grown in both, which kept only one number).
size_t KeptNumberFor(const KeptRow& k, int which);
const char* TableFileName(TableId t); // "itemstatcost" / "properties"
// The table text with the kept rows laid under its own header (rows whose name the text already has
// are left out; notes say so). Returns the rows added (in order).
// placed (may be nullptr) gets every kept row's number in the text after it: the one it was
// appended at, or the one the text already holds it at (an exported mod: "already in the mod's table").
struct Placed { const KeptRow* row; size_t number; bool already; };
std::string AppendKept(const std::string& text, TableId t, const std::vector<const KeptRow*>& rows, std::vector<std::string>& added, std::vector<std::string>& notes,
                       std::vector<Placed>* placed = nullptr);
// The kept rows whose number in the text is not the one they had when made (which: 0 excel\, 1 base\).
struct Moved { TableId table; int which; std::string name; size_t was, now; };
std::vector<Moved> MovedRows(const std::vector<Placed>& placed, TableId t, int which);

// ---- the remove journal ------------------------------------------------------------------
// remove-in-progress.txt: "itemstatcost<TAB>name<TAB>number<TAB>backup" (backup "" until Save Scan's
// fix is done). What a start does with one that is left over:
//   Ended        grown-rows.txt no longer has the row: the remove ended, only the delete was missed
//   StoppedInFix no backup yet: Save Scan's fix was running (Save Scan's own Saves tab restores)
//   Restore      the saves were fixed but the table change never landed: restore(backup)
struct JournalStep { enum What { None, Ended, StoppedInFix, Restore } what = None; std::string name, backup; size_t number = 0; };
JournalStep DecideJournal(const std::string& journal, const std::vector<KeptRow>& kept);
std::string JournalLine(const std::string& name, size_t n, const std::string& backup);
// setCells with a save resize: "setcells<TAB>name<TAB>number<TAB>backup<TAB>col=value..." - Ended when
// the kept row already holds those cells.
std::string CellsJournalLine(const std::string& name, size_t n, const std::string& backup, const std::vector<std::pair<std::string, std::string>>& cells);
// The kept row's cells set (column names case-insensitive; an empty value takes the cell out).
void SetKeptCells(KeptRow& k, const std::vector<std::pair<std::string, std::string>>& cells);
std::string KeptCell(const KeptRow& k, const std::string& column);
// At start: Live Properties' stat-edits.txt (number, signed, send bits, save bits, save add, ...,
// made name at field 17) against the kept ItemStatCost rows, by made name. A cell of how the stat is
// saved or sent (Signed, Send Bits, Save Bits, Save Add) that differs takes stat-edits' value ("-" = not set there: the kept one stays);
// those edits are what the saves were written with. -> notes, one per changed cell; true = kept changed.
bool ReconcileSaveShape(std::vector<KeptRow>& kept, const std::string& statEdits, std::vector<std::string>& notes);

// ---- one row in several data versions -------------------------------------------------
// A request for data version 0 runs on targets such as {1, 3} (1 and 2 one object) and the row has
// a number per target (found by name: it may differ). The k-th number goes with the k-th target.
struct VersionRow { int version; size_t number; };
// -> false when the two lists do not pair up (no row then)
bool PairVersions(const std::vector<int>& targets, const std::vector<size_t>& numbers, std::vector<VersionRow>& out);
// done()'s numbers[4] (index = data version, -1 = not that one): `number` for `version`, and for 2
// too when versions 1 and 2 are one tables object. Indexes outside 1..3 are left alone.
void ReportNumber(int32_t (&numbers)[4], int version, size_t number, bool oneObject12);
// One number for every target (a save resize names one stat number) -> SIZE_MAX when they differ
size_t SameNumber(const std::vector<VersionRow>& rows);

// ---- rename --------------------------------------------------------------------------
// A grown row renamed: its own name cell and every cell of the kept rows / a sheet that names it
// (ItemStatCost: maxstat, op base, op stat1-3; Properties: stat1-7 for a stat). `t` = the table of
// the renamed row. -> cells changed.
size_t RenameInKept(std::vector<KeptRow>& kept, TableId t, const std::string& was, const std::string& now);
size_t RenameInSheet(Sheet& sheet, TableId sheetTable, TableId t, const std::string& was, const std::string& now);

// ---- the model of one data version (native tests) -----------------------------------------
struct ModelTable {
	std::vector<uint8_t> bytes; // the live buffer
	size_t rows = 0;
	std::vector<std::vector<uint8_t>> retired; // earlier buffers, kept
	Sheet sheet;                // the text the table was read from (+ the rows grown into it)
};
struct Model {
	NameTable names[3];
	ModelTable stats, props;
	std::vector<uint16_t> descOrder;
	bool retailSaves = true;
	std::shared_ptr<std::map<std::string, uint16_t>> strings = std::make_shared<std::map<std::string, uint16_t>>();
	Resolver resolver();
	ModelTable& Of(TableId t) { return t == TableId::ItemStatCost ? stats : props; }
};
void Load(Model& m, const std::string& statText, const std::string& propText);
struct GrowResult { bool ok = false; size_t number = 0; bool rehash = false; std::vector<size_t> relinked, relinkedProps; std::string why; };
GrowResult Grow(Model& m, TableId t, const std::string& rowText);
// the model of Live Tables' remove (stats): refused while anything still names the row
GrowResult RemoveStat(Model& m, size_t n);
GrowResult RenameRow(Model& m, TableId t, size_t n, const std::string& now);

} // namespace lt
