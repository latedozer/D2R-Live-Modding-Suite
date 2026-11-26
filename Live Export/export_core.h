// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Export: the Live Suite's edits written into a mod's text tables, and the same edits as a
// D2RMM mod. Text in, text out; no game calls here, so it is tested natively (test/export_test.cpp).
//
// What is read (all in d2rloader\live-suite\<plugin>\<mod>\):
//   live-properties   property-edits.txt  code, then 7 lines x func / stat / set / val
//                     stat-edits.txt      number, 16 cells, the plugin's name, the game's name
//                     affix-edits.txt     kind, number, the row's name as the game read it, 35 cells
//                     string-keys.txt     string number -> key, for keys typed into stat cells
//                     made stats and properties: only the rows Live Properties' API (live_props_api.h
//                     version 2, listMade) says the game has this start, at those numbers; their
//                     cells from the files above, matched by name / code
//   live-cube-recipes recipes.txt / recipes-classic-lod.txt: the cube's whole list when it has lines
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace exporter {

struct Report {
	std::vector<std::string> done, problems;
};

struct Tsv {
	std::vector<std::string>              headers;
	std::vector<std::vector<std::string>> rows;
	int Column(const std::string& name) const; // -1: none
	std::string& At(size_t row, const std::string& column); // the cell (the row grows to the header's width)
};
bool        ParseTsv(const std::string& text, Tsv& tsv);
std::string FormatTsv(const Tsv& tsv); // CRLF, as the game's own files

constexpr int kPropCells = 28, kStatCells = 16, kAffixCells = 35;
enum AffixKind { kSuffix, kPrefix, kAutomagic };

struct PropEdit {
	std::string                          code;
	std::array<std::string, kPropCells> cells; // line n: func, stat, set, val at 4n..4n+3
};
struct StatEdit {
	int                                  id = 0;     // its row in the main tables (a made stat: its listMade number)
	int                                  baseId = 0; // its row in the base\ tables (a made stat: listMade of data version 2; -1 = not there)
	std::string                          name, was; // the plugin's name (made stats), the game's name of the row
	std::array<std::string, kStatCells> cells;
	bool                                 made = false; // a stat made by the user: it has the plugin's made name (the sure sign); else was is empty, a filler's (IsFillerName), or a made.<name> of stat-ids.txt
	using Cells = std::vector<std::pair<std::string, std::string>>;
	Cells                                grownCells;   // a row Live Tables grew: its cells as grown (grown-rows.txt), written before `cells`
	bool                                 lined = true; // `cells` are a stat-edits.txt line (false: a grown row with no line, only grownCells)
};
// A row name that marks an empty row: Exalted Items' live_free_<n> (1.3.0) and xlt_unused_<n>
// (1.2.0), Live Export 0.2.0-0.3.2's live_unused_<n> padding (0.3.3 writes none), Live Properties' live_free_<n>.
bool IsFillerName(const std::string& name);
struct AffixEdit {
	int                                   kind = kPrefix;
	size_t                                index = 0;
	std::string                           was;
	std::array<std::string, kAffixCells> cells;
};
// A row Live Properties added to a table this start (live_props_api.h MadeRow): name or code, number.
struct MadeRow {
	std::string name;
	int         number = -1;
};
enum class LiveProps { missing, old, ok }; // not installed / its API has no listMade (version < 2) / listMade answered
// A row Live Tables grew (grown-rows.txt: table<TAB>v=..<TAB>n=..<TAB>column=value...), with its
// numbers this start from Live Tables' API (find; -1 = the game does not have it there).
struct GrownRow {
	bool                                             stat = true; // itemstatcost, else properties
	std::string                                      name;        // Stat / code: its name now (Live Tables' files follow a rename)
	int                                              number = -1, baseNumber = -1;
	std::vector<std::pair<std::string, std::string>> cells;
};
enum class LiveTables { missing, old, ok }; // not installed / no isDisabled (API before version 2) / answered
// A disabled stat (Live Tables: disabled-rows.txt by name, isDisabled says it is so this start).
struct StatOff {
	std::string name;
	int         number = -1, baseNumber = -1;
};
// A disabled affix (Live Properties' affix-off.txt: "prefix 9"); `was` its name in the game's table.
struct AffixOff {
	int         kind = 0;
	size_t      index = 0;
	std::string was;
};
struct Edits {
	std::vector<PropEdit>       props;
	std::vector<std::string>    propsOff; // property codes switched off (property-off.txt): written with func1..func7 empty
	std::vector<std::string>    madeProps; // property codes made in Live Properties (made-properties.txt); after TakeMadeRows: the ones in the game, in row order
	std::vector<StatEdit>       stats;
	// Live Properties' API: what listMade says the game has this start (set by the plugin; tests set it)
	LiveProps                   liveProps = LiveProps::missing;
	std::vector<MadeRow>        madeStatRows, madeStatRowsBase, madePropRows; // stats in the main tables / base\, properties
	// Live Tables' API: the rows it grew (cells from grown-rows.txt), the stats it has disabled
	LiveTables                  liveTables = LiveTables::missing;
	std::vector<GrownRow>       grown;
	std::vector<StatOff>        statsOff;
	std::vector<AffixOff>       affixesOff;
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> madePropCells; // after TakeMadeRows: a grown property's cells
	std::set<long>              noString; // string numbers that mean "no string" (the game's answer for an unknown key: 5382 'dummy')
	std::vector<AffixEdit>      affixes;
	std::map<long, std::string> stringKeys; // string number -> key (string-keys.txt; more are added by the plugin)
	Tsv  recipes, recipesBase;              // the cube lists; empty rows = the game's own list
	bool Empty() const;
};
// Each returns the lines it could not read, for the report.
std::vector<std::string> ReadPropertyEdits(const std::string& text, Edits& edits);
std::vector<std::string> ReadStatEdits(const std::string& text, Edits& edits);
std::vector<std::string> ReadAffixEdits(const std::string& text, Edits& edits);
void                     ReadStringKeys(const std::string& text, Edits& edits);
// property-off.txt (Live Properties): one property code a line; "#" alone or "# ..." is a comment
// ("#12" is a code); trailing spaces and tabs do not count.
void                     ReadPropertyOff(const std::string& text, Edits& edits);
// made-properties.txt (Live Properties 0.9.0): '#' comments, else code<TAB>was (was: live_free_p<k>
// while made this session, else the code). Their cells are property-edits.txt's lines of the code.
void                     ReadMadeProperties(const std::string& text, Edits& edits);
// grown-rows.txt (Live Tables): the rows, numbers left -1 (the plugin asks find()). disabled-rows.txt
// (Live Tables): "itemstatcost<TAB>v=..<TAB><name>" -> statsOff, numbers -1. affix-off.txt (Live
// Properties): "prefix 9" / "suffix 3" / "automagic 1" a line. Each returns the lines it skipped.
std::vector<std::string> ReadGrownRows(const std::string& text, Edits& edits);
std::vector<std::string> ReadDisabledRows(const std::string& text, Edits& edits);
std::vector<std::string> ReadAffixOff(const std::string& text, Edits& edits);
// Keeps only the made stats and properties listMade says the game has, at its numbers (stats:
// main tables and base\), matched by name (stat-edits.txt col 17; else the row's name) / code; the
// rest are taken out and reported ("not in the game this start"), and so is every made row when
// Live Properties is missing or too old. A listed stat with no stat-edits.txt line is reported.
// Run it before LeaveOutExaltedRows.
void                     TakeMadeRows(Edits& edits, Report& report);

// The columns of a stat card's cells in itemstatcost.txt, of an affix card's in the affix tables.
extern const char* const kStatColumns[kStatCells];
extern const char* const kAffixColumns[kAffixCells];
// The cells a disabled stat has empty: no op and its links, no description, no item event (as
// Live Tables' disable makes the row in memory: op 0, descfunc 0, dgrp 0, itemevent 0xffff).
extern const char* const kStatOffColumns[10];

// A string entry to add (an affix name of the user's own words).
struct NewString {
	std::string key, text;
};

// The line export-report.txt ends with: "could not carry: <each left-out thing>; ..." or "could not carry: nothing".
std::string CouldNotCarry(const Report& report);

// The edits written into the tables. A made stat is a new row right after the table's last one
// (Stat its name, *ID, *eol 0), or the row of its number when that row already has its name; a
// made stat whose number is past the next row is refused with a reason - no filler rows, ever.
// `base`: the table is a base\ one (made stats at their baseId). Live Properties' spare rows (live_free_<n>; properties: live_free_p<n>) at a table's end are
// left out: they are empty and nothing points at them.
void ApplyProperties(Tsv& properties, const Edits& edits, Report& report);
void ApplyStats(Tsv& itemstatcost, const Edits& edits, bool base, Report& report);
void ApplyAffixes(Tsv& table, AffixKind kind, const Edits& edits, const std::function<bool(const std::string&)>& isKey, std::vector<NewString>& strings, Report& report);
bool ApplyCube(Tsv& cube, const Tsv& recipes, Report& report, const char* which); // false: the list is empty (the game's own stays)
// Strings added to a strings file (a JSON array); ids from `nextId` on.
std::string AddStrings(const std::string& json, const std::vector<NewString>& strings, long nextId);
long        HighestStringId(const std::string& json);

// ---- Exalted Items' rows (agreed with the Exalted Items session, 2026-11-23; Exalted Items 1.2.0
// IndexModRows). Its cube rows start with "[xlt] ". A mod that brings one keeps it; Exalted Items
// makes none for the same key, and enabled 0 means "removed". Keys, on K(cell) = trimmed, one
// pair of surrounding quotes off, lower case: output contains "exc" -> upgrade, key K(input 1);
// else contains "eli" -> back to elite, key K(input 1) up to the first comma; else restore, the same.
bool        IsExalted(const std::string& description); // starts with exactly "[xlt] "
std::string ExaltedKey(const std::string& output, const std::string& input1); // "upgrade:..", "downgrade:..", "restore:.."
// ---- the tab's buttons (Mod Book's action row: slots 232 wide, 4 to a row)
struct ActionButton {
	const char* click;
	const char* text;
	const char* color;
	int         slots;
};
extern const ActionButton kExportButtons[3]; // The whole mod (.mpq), .mpq without Exalted copies, A D2RMM mod
// Does a label fit a button of `slots` slots without wrapping out of it? At most 12 letters in one
// slot, 28 in two (Mod Book's finding), 36 in three, 38 in four.
bool LabelFits(const std::string& text, int slots);

// Exalted Items' stat-ids.txt (1.3.0 and later; <loader>\exalted-items\<mod>\stat-ids.txt): which
// stat numbers it holds. key=value lines, '#' comments: layout=packed|legacy, marker=<n>,
// roll.<k>=<n>, free=<n> (an empty row, live_free_<n>), made.<name>=<n> (a Live Properties made
// stat; its row carries that name).
struct ExaltedIds {
	bool                       legacy = false;
	std::vector<int>           own;  // marker and rolls: Exalted Items makes these rows
	std::vector<int>           free; // live_free_ rows
	std::map<std::string, int> made;
};
// False (why says so): not understood; then the file is not used.
bool ReadExaltedIds(const std::string& text, ExaltedIds& ids, std::string& why);
// Sets StatEdit::made: a filler's name or no name, or a made.<name> of the map at its number.
void MarkMadeStats(Edits& edits, const ExaltedIds* ids);
// The rows Exalted Items makes again at every start on a plain mod (its stats, its xlt-
// properties): edits to them are left out and reported. With stat-ids.txt (ids): its marker and
// roll numbers and every row named xlt_* other than the xlt_unused_ fillers. Without it
// (Exalted Items before 1.3.0): rows named xlt_*, and made stats past 486 (its stats started at 487).
// Made stats (MarkMadeStats, done here too) are carried.
void LeaveOutExaltedRows(Edits& edits, Report& report, const ExaltedIds* ids = nullptr);
// The plain .mpq's cube list: the user's list without Exalted Items' rows the user did not
// change; a changed one is kept, a removed one is written with enabled 0. `madeAgainst`: the
// list the recipes were made against (Exalted Items' cubemain copy).
bool ApplyCubePlain(Tsv& cube, const Tsv& recipes, const Tsv& madeAgainst, Report& report, const char* which);

// The D2RMM mod: mod.json and mod.ts doing the same to whatever tables D2RMM hands it.
// `baseCube` / `baseCubeBase`: the lists the recipes were made against (for what was added
// and removed).
std::string D2rmmJson(const std::string& name, const std::string& version, const std::string& description);
std::string D2rmmTs(const Edits& edits, const Tsv& baseCube, const Tsv& baseCubeBase, const std::function<bool(const std::string&)>& isKey, Report& report);

} // namespace exporter
