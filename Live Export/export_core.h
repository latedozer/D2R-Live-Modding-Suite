// Live Export: the Live Suite's edits written into a mod's text tables, and the same edits as a
// D2RMM mod. Text in, text out; no game calls here, so it is tested natively (test/export_test.cpp).
//
// What is read (all in d2rloader\live-suite\<plugin>\<mod>\):
//   live-properties   property-edits.txt  code, then 7 lines x func / stat / set / val
//                     stat-edits.txt      number, 16 cells, the plugin's name, the game's name
//                     affix-edits.txt     kind, number, the row's name as the game read it, 35 cells
//                     string-keys.txt     string number -> key, for keys typed into stat cells
//   live-cube-recipes recipes.txt / recipes-classic-lod.txt: the cube's whole list when it has lines
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace exporter {

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
	int                                  id = 0;
	std::string                          name, was; // the plugin's name (made stats), the game's name of the row
	std::array<std::string, kStatCells> cells;
};
struct AffixEdit {
	int                                   kind = kPrefix;
	size_t                                index = 0;
	std::string                           was;
	std::array<std::string, kAffixCells> cells;
};
struct Edits {
	std::vector<PropEdit>       props;
	std::vector<StatEdit>       stats;
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

// The columns of a stat card's cells in itemstatcost.txt, of an affix card's in the affix tables.
extern const char* const kStatColumns[kStatCells];
extern const char* const kAffixColumns[kAffixCells];

// A string entry to add (an affix name of the user's own words).
struct NewString {
	std::string key, text;
};

struct Report {
	std::vector<std::string> done, problems;
};

// The edits written into the tables. `padStats`: a made stat whose row the table does not have
// (a table without Exalted Items' filler rows) gets empty rows up to it, named live_unused_<n>.
void ApplyProperties(Tsv& properties, const Edits& edits, Report& report);
void ApplyStats(Tsv& itemstatcost, const Edits& edits, bool padStats, Report& report);
void ApplyAffixes(Tsv& table, AffixKind kind, const Edits& edits, const std::function<bool(const std::string&)>& isKey, std::vector<NewString>& strings, Report& report);
bool ApplyCube(Tsv& cube, const Tsv& recipes, Report& report, const char* which); // false: the list is empty (the game's own stays)
// Strings added to a strings file (a JSON array); ids from `nextId` on.
std::string AddStrings(const std::string& json, const std::vector<NewString>& strings, long nextId);
long        HighestStringId(const std::string& json);

// The D2RMM mod: mod.json and mod.ts doing the same to whatever tables D2RMM hands it.
// `baseCube` / `baseCubeBase`: the lists the recipes were made against (for what was added
// and removed).
std::string D2rmmJson(const std::string& name, const std::string& version, const std::string& description);
std::string D2rmmTs(const Edits& edits, const Tsv& baseCube, const Tsv& baseCubeBase, const std::function<bool(const std::string&)>& isKey, Report& report);

} // namespace exporter
