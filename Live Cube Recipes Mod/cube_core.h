// Live Cube Recipes: the part that knows nothing about Windows or the running game.
//
// It reads a recipe file (cubemain.txt columns), turns one line into the record the game keeps
// for a recipe, and lays the user's recipes over the game's recipe list.
//
// The record layout is the game's (D2R image stamp 0x6ab3782c as widened by D2RLoader 1.3:
// 0x16c bytes a recipe). The columns below are the table the game's own cubemain loader hands
// to its table reader (0x3e3ff0); their offsets are the game's 0x158-byte ones, and Offset()
// moves them the way D2RLoader's LoadExcelTable does. The two hard cell kinds (inputs and
// outputs) are not parsed here at all: they go to the game's own parsers through `Game`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cube {

constexpr size_t kWideRow  = 0x16c; // a recipe in a game started through D2RLoader 1.3
constexpr size_t kStockRow = 0x158; // a recipe in the game's own cubemain.bin (tests only)

enum class Kind : uint8_t {
	Byte,   // number, 1 byte
	Word,   // number, 2 bytes
	Dword,  // number, 4 bytes
	Param,  // number, or a name the game looks up (4 bytes)
	Class,  // class code such as "ama" (1 byte, 0xff = any class)
	Mod,    // property code (4 bytes, 0xffffffff = none)
	Input,  // "input N" cell: the game's parser
	Output, // "output" cell: the game's parser
};

struct Column {
	const char* name;
	Kind        kind;
	uint16_t    at; // offset in the 0x158-byte record; for Input/Output the slot number
};

extern const Column kColumns[];
extern const size_t kColumnCount;

struct Layout {
	size_t rowSize;
	bool   wide;
	// What a blank number cell does to a byte the output parser has already set (it sets the
	// item level of a unique or set output): false = the blank cell makes it 0, true = it stays.
	bool blankKeeps = false;
};
constexpr Layout kWide { kWideRow, true };
constexpr Layout kStock { kStockRow, false };

// Where a field of the 0x158-byte record is in this layout.
size_t Offset(const Layout& layout, size_t stockOffset);
// True for the bytes two compiles of the same line may differ in: each output slot starts with
// the line number it was read from.
bool IsLineNumberByte(const Layout& layout, size_t offset);
bool SameRecipe(const Layout& layout, const uint8_t* a, const uint8_t* b);
// The enabled flag is the first byte of a recipe.

// What the compiler asks of the game. Texts are private copies the callee may change.
struct Game {
	virtual ~Game() = default;
	// false: the cell does not name an item, an item type or a unique/set item
	virtual bool Input(char* text, uint8_t* row, int slot, int line)  = 0;
	virtual bool Output(char* text, uint8_t* row, int slot, int line) = 0;
	// writes the 4 bytes at `offset`; false: no property has that code
	virtual bool Mod(const char* text, uint8_t* row, size_t offset) = 0;
	// class number, or -1 when no class has that code
	virtual int Class(const char* text) = 0;
	// a name in the param column
	virtual uint32_t Param(const char* text) = 0;
};

// The first line of cubemain.txt: which cell holds which column.
struct Header {
	std::vector<int> cell;             // per kColumns entry: its cell, or -1 when the file lacks it
	int              description = -1; // the description cell, for messages
	int              action      = -1; // the "*action" cell of the plugin's own files ("remove")
	bool             complete    = false; // has a "*complete" column: the file is the cube's whole list
	int              known       = 0;  // how many of the game's columns the file has
};
Header ReadHeader(std::string_view line);

struct Sheet {
	std::string                   text;       // the whole file; the views below point into it
	std::string_view              headerLine;
	Header                        header;
	std::vector<std::string_view> lines;      // data lines, blank ones included, in file order
	bool                          usable = false; // has the columns a recipe needs
};
// `text` is moved into the sheet.
Sheet ReadSheet(std::string text);
bool  IsBlank(std::string_view line);
// "line 12 (3 Thul Runes + ...)": how messages name a line (1 = the header).
std::string Describe(const Sheet& sheet, size_t line);

// One line of the file as a recipe record (`row` holds layout.rowSize bytes). `line` is the
// number the game would give it: 0 for the first data line. On false, `problem` says which cell
// the game refused, and the record must not be used.
// `sensitive` (optional) is set when the record depends on layout.blankKeeps.
bool CompileLine(Game& game, const Layout& layout, const Header& header, std::string_view text, int line, uint8_t* row, std::string& problem, bool* sensitive = nullptr);


// Compiled lines are kept between builds, keyed by the cells the record depends on, so every
// sheet shares them whatever its columns and descriptions are (empty = the game refused it).
using Cache = std::unordered_map<std::string, std::vector<uint8_t>>;

// What became of a line of the user's file.
enum class LineStatus : int8_t {
	Blank    = 0,
	Added    = 1, // a recipe with ingredients no recipe of the game has
	Replaced = 2, // stands where a recipe of the game with the same ingredients stood
	Removed  = 3, // "*action" = remove: took recipes of the game out
	Idle     = 4, // "*action" = remove, but no recipe of the game has those ingredients
	Refused  = 5, // the game could not read it
};

struct Outcome {
	std::vector<uint8_t>     rows; // the list the game should have now
	size_t                   count    = 0;
	int                      added    = 0; // the user's recipes with ingredients no recipe of the game has
	int                      replaced = 0; // the user's recipes standing where a recipe of the game stood
	int                      disabled = 0; // of those two, the ones with enabled = 0 (in the list, switched off)
	int                      removed  = 0; // remove lines that took recipes of the game out
	int                      covered  = 0; // recipes of the game out of the list in all
	int                      idle     = 0; // remove lines that match no recipe of the game
	int                      refused  = 0; // lines the game could not read
	std::vector<LineStatus>  status;       // per line of the file
	std::vector<std::string> problems;
};

// True when two recipes ask for the same thing: everything in front of the outputs but the
// enabled flag (difficulty, class, op/param/value, number of inputs, version, ladder seasons and
// the seven inputs).
bool SameIngredients(const Layout& layout, const uint8_t* a, const uint8_t* b);

// The game's list with the user's recipes laid over it:
//  * a line whose ingredients equal a recipe of the game stands in its place (every recipe of
//    the game with those ingredients goes). With enabled = 0 it stands there switched off;
//  * a line with "remove" in the "*action" column takes the game's recipes with its ingredients
//    out and puts nothing in their place;
//  * every other line is a new recipe, put in front of the game's (the cube takes the first
//    recipe that fits), in the file's order.
// `gameRows` is never written to.
Outcome Overlay(Game& game, const Layout& layout, const uint8_t* gameRows, size_t gameCount, const Sheet& mine, Cache& cache);

// The first line of a recipe file with every column the game reads, for a new file.
std::string FullHeader();

// Every recipe of a list as text, in the list's order, under FullHeader(): one line a recipe,
// ready to be copied into a recipe file.
// A record holds no text, so the text is looked for: every line of `sources` (recipe files
// known to have gone into the list: the user's, the mod's cubemain.txt, other plugins' copies)
// is compiled, and a line that gives exactly a record's bytes is that record's text. Earlier
// sources win. A record no line gives is written as a line with only a description saying so.
struct Listing {
	std::string          text;
	std::vector<int>     mine;    // per recipe: the line of sources[0] (the user's file) its text is from, or -1
	std::vector<uint8_t> hasText; // per recipe: 1 when a line was found for it
	size_t           found   = 0;
	size_t           missing = 0;
};
Listing ListAll(Game& game, const Layout& layout, const uint8_t* rows, size_t count, const std::vector<const Sheet*>& sources, Cache& cache);

// A recipe as the in-game recipe book shows it.
struct Shown {
	std::string title;  // the description
	std::string detail; // the result first, then what goes in: "r11  <-  r10,qty=3 + gcy"
	std::string search; // both, lower case
};
// One entry per non-blank line of a recipe sheet (all-recipes.txt's text).
std::vector<Shown> Summaries(const Sheet& sheet);
// The entry of one line.
Shown Summary(const Sheet& sheet, size_t line);
// The entries a search text leaves: every word of it must be in the entry (any case).
std::vector<uint32_t> Search(const std::vector<Shown>& all, std::string_view words);

// legend.txt: what the words and codes in recipes mean.
//  * the words the game's recipe parser knows in input and output cells (fixed text);
//  * every item type code with its name, what it is a kind of and which types it includes;
//  * every item code with its name and type; every property code.
// The three lists are read from the texts of the tables given (itemtypes.txt; weapons.txt,
// armor.txt and misc.txt; properties.txt; itemstatcost.txt - a table may come in several texts: the mod's and
// other plugins' copies; a code keeps the first name found for it).
struct LegendTables {
	std::vector<std::string> types, items, properties, stats; // stats: itemstatcost.txt
};
// For the in-game legend panel the same legend as entries, in two tabs (both optional):
// `codes` = the recipe words and the item type, item and property codes;
// `values` = the columns and what their values mean (every op, param's stat numbers, ...).
struct Shown;
std::string Legend(const LegendTables& tables, std::vector<Shown>* codes = nullptr, std::vector<Shown>* values = nullptr);

// A recipe line as cells, for the in-game editor: the description, one cell per kColumns entry,
// and whether it is a remove line.
struct Cells {
	std::string              description;
	std::vector<std::string> cell; // kColumnCount of them
	bool                     remove = false;
};
Cells       CellsOf(const Sheet& sheet, size_t line);
std::string LineOf(const Cells& cells); // under FullHeader(), without a line end
// The same ingredients, by the cells' text (every column in front of the outputs but enabled).
bool SameIngredientCells(const Cells& a, const Cells& b);
// What numinputs has to be for these inputs: one for each, or its qty.
int CountInputs(const Cells& cells);
// The place of a column in Cells::cell, or -1.
int ColumnIndex(const char* name);

// A description with the result first: "A + B -> C" becomes "C <- A + B". A description without
// " -> ", or with a " <- " already, stays as it is.
std::string ResultFirst(std::string_view description);

// Names for codes, from the mod's tables (the legend's): item code -> name, item type code -> name.
struct Names {
	std::unordered_map<std::string, std::string> items, types;
};
struct LegendTables;
Names ReadNames(const LegendTables& tables);
// A description made from a recipe's cells, the result first: "Amn Rune <- 3 Thul Rune + Chipped Topaz".
// Codes without a name stay as they are.
std::string GenericName(const Cells& cells, const Names& names);

// A button picture of the size asked for, made from one of the game's own ("SpA1": a 40-byte
// header, then RGBA rows with the frames side by side). Every frame is scaled to `height`, keeps
// its two ends and has its middle repeated (mirrored) to fill `width`. The result has the same
// frames, side by side without gaps. Empty when `source` is not such a picture.
std::string ButtonSprite(std::string_view source, int width, int height);

// ---- recipes.txt as the cube's whole list (a file whose first line has a "*complete" column)

// The first line of such a file.
std::string CompleteHeader();

// The cube's list when the user's file is the whole list: every line of the file that the game
// can read, in the file's order (enabled or not), then `extra`: records of the game no recipe
// file has text for, which a file therefore cannot hold. Remove lines of older files are skipped.
Outcome ExactList(Game& game, const Layout& layout, const Sheet& mine, Cache& cache, const uint8_t* extra, size_t extraCount);

// The user's list against the defaults (the recipes of the mod and the game, as text).
// "The same recipe" here means the same ingredients (see SameIngredients).
struct Compared {
	std::vector<size_t>  fresh;   // lines of the defaults that are not in the user's list and were not deleted
	std::vector<size_t>  deleted; // lines of the defaults that are not in the user's list and are noted as deleted
	std::vector<uint8_t> own;     // per line of the user's list: 1 = no default is exactly this recipe (changed or made by the user)
	std::vector<uint8_t> isDefault; // per line of the user's list: 1 = a default has these ingredients
};
// The defaults, looked up by record: built once, asked for every line of the user's list.
// It points into `cache`, which must outlive it and must not lose entries.
struct DefaultsIndex {
	std::unordered_map<uint64_t, std::vector<const uint8_t*>> ingredients, whole;
};
DefaultsIndex IndexDefaults(Game& game, const Layout& layout, const Sheet& defaults, Cache& cache);
// One line of the user's list: what ExactList makes of it (Blank, Idle, Refused or Added) and
// what Compare says of it (own, isDefault).
struct LineFacts {
	LineStatus status    = LineStatus::Blank;
	bool       own       = false;
	bool       isDefault = false;
};
LineFacts FactsOf(Game& game, const Layout& layout, const DefaultsIndex& index, const Sheet& mine, size_t line, Cache& cache);

// `cache` is one for all three sheets (kept between calls).
Compared Compare(Game& game, const Layout& layout, const Sheet& defaults, const Sheet& mine, const Sheet& deleted, Cache& cache);

}
