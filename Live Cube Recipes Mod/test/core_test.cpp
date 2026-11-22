// Checks of cube_core that need no game:
//  1. every line of the game's own cubemain.txt compiles to the record in the game's own
//     cubemain.bin (both data sets), apart from the cells the game's parsers fill in;
//  2. the overlay: new recipes, recipes in place of the game's, removals, bad lines.
// usage: core_test <folder with the game's data/global/excel files>
#include "../cube_core.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace cube;

static int g_failed = 0;
#define CHECK(what, ...)                                  \
	do {                                                  \
		if (what) {                                       \
			printf("ok    ");                             \
		} else {                                          \
			printf("FAIL  ");                             \
			++g_failed;                                   \
		}                                                 \
		printf(__VA_ARGS__);                              \
		printf("\n");                                     \
	} while (0)

static std::string ReadFile(const std::string& path) {
	std::ifstream     in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

static std::vector<std::string> ColumnOf(const std::string& text, const char* column, bool skipExpansion) {
	std::vector<std::string> out;
	std::stringstream        in(text);
	std::string              line;
	int                      at = -1;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		std::vector<std::string> cells;
		std::stringstream        split(line);
		for (std::string cell; std::getline(split, cell, '\t');) {
			cells.push_back(cell);
		}
		if (at < 0) {
			for (size_t i = 0; i < cells.size(); ++i) {
				if (strcasecmp(cells[i].c_str(), column) == 0) {
					at = static_cast<int>(i);
				}
			}
			continue;
		}
		if (skipExpansion && !cells.empty() && cells[0] == "Expansion") {
			continue;
		}
		out.push_back(static_cast<size_t>(at) < cells.size() ? cells[static_cast<size_t>(at)] : "");
	}
	return out;
}

// The game for the stock check: property and class codes from the game's own files; the two
// parsers do nothing (their part of the record is not compared).
struct StockGame : Game {
	std::vector<std::string> properties, classes;
	bool Input(char*, uint8_t*, int, int) override { return true; }
	bool Output(char*, uint8_t*, int, int) override { return true; }
	bool Mod(const char* text, uint8_t* row, size_t offset) override {
		for (size_t i = 0; i < properties.size(); ++i) {
			if (properties[i] == text) {
				row[offset]     = static_cast<uint8_t>(i);
				row[offset + 1] = static_cast<uint8_t>(i >> 8);
				row[offset + 2] = row[offset + 3] = 0;
				return true;
			}
		}
		return false;
	}
	int Class(const char* text) override {
		for (size_t i = 0; i < classes.size(); ++i) {
			if (classes[i] == text) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
	uint32_t Param(const char*) override { return 0; }
};

static void StockCheck(const std::string& root, const char* bank) {
	const std::string folder = root + "/" + bank;
	const std::string bin    = ReadFile(folder + "cubemain.bin");
	Sheet             sheet  = ReadSheet(ReadFile(folder + "cubemain.txt"));
	if (bin.size() < 4 || sheet.lines.empty()) {
		printf("skip  %scubemain: the game's files are not in %s\n", bank, root.c_str());
		return;
	}
	StockGame game;
	std::string props = ReadFile(folder + "Properties.txt");
	if (props.empty()) {
		props = ReadFile(folder + "properties.txt");
	}
	game.properties = ColumnOf(props, "code", true);
	game.classes    = ColumnOf(ReadFile(root + "/base/PlayerClass.txt"), "code", true);
	uint32_t count  = 0;
	memcpy(&count, bin.data(), 4);
	CHECK(count == sheet.lines.size() && bin.size() == 4 + count * kStockRow, "%scubemain: %u records for %zu lines", bank, count, sheet.lines.size());
	CHECK(sheet.usable && sheet.header.known == static_cast<int>(kColumnCount), "%scubemain: all %zu columns found in the first line", bank, kColumnCount);

	int         wrong = 0;
	uint8_t     row[kStockRow];
	std::string problem;
	for (size_t i = 0; i < sheet.lines.size() && i < count; ++i) {
		const uint8_t* theirs = reinterpret_cast<const uint8_t*>(bin.data()) + 4 + i * kStockRow;
		bool           same   = CompileLine(game, kStock, sheet.header, sheet.lines[i], static_cast<int>(i), row, problem);
		for (size_t at = 0; same && at < kStockRow; ++at) {
			const bool inputs  = at >= 0x14 && at < 0x4c;                             // the input parser's
			const size_t in    = at >= 0x4c ? (at - 0x4c) % 0x59 : 0x59;
			const bool outputs = in < 0x1d && in != 0xe && in != 0xf && in != 0x10;   // the output parser's (0xe-0x10 are lvl, plvl, ilvl)
			if (!inputs && !outputs && row[at] != theirs[at]) {
				same = false;
				if (wrong < 5) {
					printf("      line %zu: byte 0x%zx is %02x, the game's is %02x\n", i + 2, at, row[at], theirs[at]);
				}
			}
		}
		wrong += same ? 0 : 1;
	}
	CHECK(wrong == 0, "%scubemain: %zu lines compile to the game's own records (%d differ)", bank, sheet.lines.size(), wrong);
}

// The game for the builder checks: an input or output is its text, copied into its slot.
struct ToyGame : Game {
	int compiles = 0;
	bool Input(char* text, uint8_t* row, int slot, int) override {
		if (strncmp(text, "bad", 3) == 0) {
			return false;
		}
		strncpy(reinterpret_cast<char*>(row) + 0x14 + slot * 10, text, 10);
		return true;
	}
	bool Output(char* text, uint8_t* row, int slot, int line) override {
		if (strncmp(text, "bad", 3) == 0) {
			return false;
		}
		uint8_t* at = row + 0x5a + slot * 0x5b;
		memcpy(at, &line, 4);
		strncpy(reinterpret_cast<char*>(at) + 4, text, 9);
		++compiles;
		return true;
	}
	bool Mod(const char* text, uint8_t* row, size_t offset) override {
		if (strcmp(text, "nope") == 0) {
			return false;
		}
		row[offset] = static_cast<uint8_t>(text[0]);
		row[offset + 1] = row[offset + 2] = row[offset + 3] = 0;
		return true;
	}
	int      Class(const char* text) override { return strcmp(text, "ama") == 0 ? 0 : -1; }
	uint32_t Param(const char*) override { return 77; }
};

static const char* kHead = "description\tenabled\tnuminputs\tinput 1\tinput 2\toutput\tmod 1\tclass\t*action\t*eol\n";

static std::string Line(const char* about, const char* in1, const char* out, const char* mod = "", const char* cls = "") {
	return std::string(about) + "\t1\t1\t" + in1 + "\t\t" + out + "\t" + mod + "\t" + cls + "\t\t0\n";
}

// The outputs of a list, in order, as "a,b,c".
static std::string Outputs(const std::vector<uint8_t>& rows, size_t count) {
	std::string out;
	for (size_t r = 0; r < count; ++r) {
		out += (r != 0 ? "," : "") + std::string(reinterpret_cast<const char*>(rows.data()) + r * kWideRow + 0x5a + 4);
	}
	return out;
}

// A line that takes the game's recipes with this ingredient out.
static std::string Remove(const char* about, const char* in1) {
	return std::string(about) + "\t1\t1\t" + in1 + "\t\t\t\t\tremove\t0\n";
}

static void OverlayChecks() {
	ToyGame              game;
	std::string          problem;
	std::vector<uint8_t> row(kWideRow), list;
	size_t               count = 0;
	const auto           add   = [&](const std::string& text, int line) {
        const Sheet one = ReadSheet(std::string(kHead) + text);
        CompileLine(game, kWide, one.header, one.lines[0], line, row.data(), problem);
        list.insert(list.end(), row.begin(), row.end());
        ++count;
	};
	// the game's list: four recipes; "c" exists twice with the same ingredient
	add(Line("A", "a1", "a"), 0);
	add(Line("B", "b1", "b"), 1);
	add(Line("C", "c1", "c"), 2);
	add(Line("C again", "c1", "c2"), 3);
	add(Line("D", "d1", "d"), 4);
	const std::vector<uint8_t> untouched = list;

	Cache   cache;
	Outcome out = Overlay(game, kWide, list.data(), count, ReadSheet(std::string(kHead)), cache);
	CHECK(out.count == 5 && out.rows == list, "an empty file: the game's list as it is");

	// a new recipe, a recipe in place of the game's "b", the game's "c" (both) removed
	Sheet mine = ReadSheet(std::string(kHead) + Line("New", "n1", "n") + "\t\t\t\t\t\t\t\t\t\n" + Line("B mine", "b1", "b+") + Remove("C off", "c1"));
	out        = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "n,a,b+,d" && out.added == 1 && out.replaced == 1 && out.removed == 1 && out.covered == 3 && out.refused == 0, "new in front, one in place, two removed: %s (%d/%d/%d/%d)",
		Outputs(out.rows, out.count).c_str(), out.added, out.replaced, out.removed, out.covered);
	CHECK(list == untouched, "the game's list was not written to");
	const int before = game.compiles;
	out              = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(game.compiles == before && Outputs(out.rows, out.count) == "n,a,b+,d", "the same file again: nothing compiled twice");

	// two of the user's lines with the same ingredients stay together, in the file's order
	mine = ReadSheet(std::string(kHead) + Line("B one", "b1", "b+") + Line("New", "n1", "n") + Line("B two", "b1", "b++") + Line("New twice", "n1", "n2"));
	out  = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "n,n2,a,b+,b++,c,c2,d", "same ingredients twice: %s", Outputs(out.rows, out.count).c_str());

	// lines the game cannot read are left out and named; a removal that matches nothing is counted
	mine = ReadSheet(std::string(kHead) + Line("Bad in", "bad", "q") + Line("Bad out", "q1", "bad") + Line("Bad mod", "q1", "q", "nope") + Line("Bad class", "q1", "q", "", "zzz") + Line("Good", "g1", "g", "m", "ama")
		+ Remove("Off", "nothing"));
	out = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "g,a,b,c,c2,d" && out.refused == 4 && out.problems.size() == 4 && out.idle == 1, "four bad lines left out: %s", Outputs(out.rows, out.count).c_str());
	for (const std::string& text : out.problems) {
		printf("      %s\n", text.c_str());
	}
	CHECK(out.problems[0].find("line 2 (Bad in): input 1") == 0, "a problem names its line and cell");

	// enabled = 0: the recipe stays in its place, switched off (the cube skips it); a new one too
	mine = ReadSheet(std::string(kHead) + Line("B off", "b1", "b").replace(6, 1, "0") + Line("New off", "n1", "n").replace(8, 1, "0"));
	out  = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "n,a,b,c,c2,d" && out.rows[0] == 0 && out.rows[2 * kWideRow] == 0 && out.rows[kWideRow] == 1 && out.disabled == 2 && out.replaced == 1 && out.added == 1
			&& out.status[0] == LineStatus::Replaced && out.status[1] == LineStatus::Added,
		"enabled = 0: in the list, switched off: %s", Outputs(out.rows, out.count).c_str());
	mine = ReadSheet(std::string(kHead) + Remove("gone", "c1") + Remove("nothing there", "zz") + Line("Bad", "bad", "q"));
	out  = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(out.status[0] == LineStatus::Removed && out.status[1] == LineStatus::Idle && out.status[2] == LineStatus::Refused && Outputs(out.rows, out.count) == "a,b,d", "every line's fate is told");

	// the editor's helpers: a line as cells and back, ingredients by text, numinputs
	{
		const Sheet full = ReadSheet(FullHeader() + "\nMy recipe\t1\t\t\t\t18\t\t\t\t4\t100\t\"r10,qty=3\"\tgcy\t\t\t\t\t\tr11\n");
		Cells       cells = CellsOf(full, 0);
		CHECK(cells.description == "My recipe" && cells.cell[static_cast<size_t>(ColumnIndex("input 1"))] == "\"r10,qty=3\"" && cells.cell[static_cast<size_t>(ColumnIndex("output"))] == "r11" && !cells.remove
				&& CountInputs(cells) == 4,
			"a line as cells; its inputs count %d", CountInputs(cells));
		const Sheet back = ReadSheet(FullHeader() + "\n" + LineOf(cells) + "\n");
		Cells       again = CellsOf(back, 0);
		CHECK(again.description == cells.description && again.cell == cells.cell && SameIngredientCells(cells, again), "cells back to a line and to cells again: the same");
		again.cell[static_cast<size_t>(ColumnIndex("output"))]  = "r12";
		again.cell[static_cast<size_t>(ColumnIndex("enabled"))] = "0";
		const bool sameStill = SameIngredientCells(cells, again);
		again.cell[static_cast<size_t>(ColumnIndex("input 2"))] = "gcb";
		again.remove = true;
		CHECK(sameStill && !SameIngredientCells(cells, again) && CellsOf(ReadSheet(FullHeader() + "\n" + LineOf(again) + "\n"), 0).remove, "another output is the same ingredients, another input is not; a remove line stays one");
	}

	// a class-only difference is another recipe
	mine = ReadSheet(std::string(kHead) + Line("A for amazons", "a1", "a-ama", "", "ama"));
	out  = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "a-ama,a,b,c,c2,d" && out.added == 1, "another class: a recipe of its own");

	// a file with other columns
	mine = ReadSheet("enabled\tdescription\tnuminputs\tinput 1\toutput\n1\tB\t1\tb1\tbb\n");
	out  = Overlay(game, kWide, list.data(), count, mine, cache);
	CHECK(Outputs(out.rows, out.count) == "a,bb,c,c2,d" && out.replaced == 1, "columns in another order: %s", Outputs(out.rows, out.count).c_str());

	// the whole list as text: the user's lines, the mod's, another plugin's copy; one record nobody has a line for
	{
		add(Line("X", "x1", "x"), 77); // in the game's list, from another plugin's file
		const std::string userText = std::string(kHead) + Line("New", "n1", "n") + Line("B mine", "b1", "b+");
		const Sheet       user     = ReadSheet(userText);
		const Sheet       modFile  = ReadSheet("enabled\tdescription\tnuminputs\tinput 1\toutput\n1\tA\t1\ta1\ta\n1\tB\t1\tb1\tb\n1\tC\t1\tc1\tc\n1\tD\t1\td1\td\n");
		const Sheet       other    = ReadSheet(std::string(kHead) + Line("A", "a1", "a") + Line("X of the other plugin", "x1", "x"));
		out                        = Overlay(game, kWide, list.data(), count, user, cache);
		Cache         listCache;
		const Listing all  = ListAll(game, kWide, out.rows.data(), out.count, { &user, &modFile, &other }, listCache);
		const Sheet   back = ReadSheet(all.text);
		std::string   names;
		for (size_t i = 0; i < back.lines.size(); ++i) {
			names += (i != 0 ? "|" : "") + std::string(back.lines[i].substr(0, back.lines[i].find('\t')));
		}
		CHECK(all.found == 6 && all.missing == 1 && back.lines.size() == 7 && all.text.rfind(FullHeader() + "\r\n", 0) == 0, "all recipes as text: 6 found, 1 without a line (%zu/%zu)", all.found, all.missing);
		CHECK(all.mine == (std::vector<int> { 0, -1, 1, -1, -1, -1, -1 }), "each listed recipe knows whether it is a line of the user's file");
		CHECK(names.find("New|A|B mine|C|(recipe 5 of the cube's list") == 0 && names.find("|D|X of the other plugin") != std::string::npos, "in the list's order, each with its own file's description: %s", names.c_str());
		// the text is as good as the original: laid over an empty game it gives the same records
		Cache         again;
		const Outcome round = Overlay(game, kWide, nullptr, 0, back, again);
		bool          same  = round.count == 7; // the line without text comes back as a switched-off blank
		for (size_t i = 0; same && i < out.count; ++i) {
			if (i != 4) {
				same = SameRecipe(kWide, round.rows.data() + i * kWideRow, out.rows.data() + i * kWideRow);
			}
		}
		CHECK(same, "the lines written out compile back to the same recipes");
		count -= 1;
		list.resize(count * kWideRow);
	}

	// recipes.txt as the whole list, and the comparison with the defaults
	{
		const std::string head = std::string(kHead);
		const Sheet defaults = ReadSheet(head + Line("A", "a1", "a") + Line("B", "b1", "b") + Line("C", "c1", "c") + Line("D", "d1", "d"));
		// the user's list: B changed (another output), C gone and noted, D gone without a note, a recipe of their own, one switched off, one unreadable
		const Sheet whole = ReadSheet(std::string(kHead).insert(strlen(kHead) - 1, "\t*complete") + Line("Mine", "m1", "m") + Line("A", "a1", "a").replace(2, 1, "0") + Line("B mine", "b1", "b+") + Line("Bad", "bad", "q"));
		const Sheet notes = ReadSheet(head + Line("C", "c1", "c"));
		CHECK(whole.header.complete && !defaults.header.complete && ReadSheet(CompleteHeader() + "\n").header.complete && ReadSheet(CompleteHeader() + "\n").usable, "a file with a *complete column is a whole list");
		Cache       exactCache, compareCache;
		const uint8_t extra[kWideRow] = { 1, 0, 0xff };
		Outcome exact = ExactList(game, kWide, whole, exactCache, extra, 1);
		CHECK(exact.count == 4 && Outputs(exact.rows, 3) == "m,a,b+" && exact.rows[kWideRow] == 0 && exact.rows[3 * kWideRow + 2] == 0xff && exact.added == 3 && exact.disabled == 1 && exact.refused == 1
				&& exact.status[3] == LineStatus::Refused && exact.problems.size() == 1,
			"the whole list: the file's lines in order (one switched off), the unreadable one out, the game's textless record kept (%s)", Outputs(exact.rows, 3).c_str());
		const Compared compared = Compare(game, kWide, defaults, whole, notes, compareCache);
		CHECK(compared.fresh == std::vector<size_t> { 3 } && compared.deleted == std::vector<size_t> { 2 }, "against the defaults: D is new to the list, C was deleted (B and A are there, by their ingredients)");
		CHECK(compared.own == (std::vector<uint8_t> { 1, 1, 1, 1 }) && compared.isDefault == (std::vector<uint8_t> { 0, 1, 1, 0 }), "which lines are the user's own work, and which stand for a default");
		{ // line by line against an index of the defaults: the same answers
			const DefaultsIndex index = IndexDefaults(game, kWide, defaults, compareCache);
			bool                agree = true;
			for (size_t i = 0; i < whole.lines.size(); ++i) {
				const LineFacts facts = FactsOf(game, kWide, index, whole, i, compareCache);
				agree = agree && facts.status == exact.status[i] && facts.own == (compared.own[i] != 0) && facts.isDefault == (compared.isDefault[i] != 0);
			}
			CHECK(agree && whole.lines.size() == 4, "asked line by line against an index of the defaults, every line gets the answers of the whole comparison");
		}
		const Sheet same = ReadSheet(std::string(kHead).insert(strlen(kHead) - 1, "\t*complete") + Line("A", "a1", "a") + Line("B", "b1", "b"));
		const Compared untouched = Compare(game, kWide, defaults, same, ReadSheet(head), compareCache);
		CHECK(untouched.own == (std::vector<uint8_t> { 0, 0 }) && untouched.fresh == (std::vector<size_t> { 2, 3 }) && untouched.deleted.empty(), "a line exactly like a default is not marked as the user's");
	}

	// the recipe book's entries and its search
	{
		const Sheet book = ReadSheet("description\tenabled\tnuminputs\tinput 1\tinput 2\toutput\toutput b\n3 Thul Runes + 1 Chipped Topaz -> Amn Rune\t1\t4\t\"r10,qty=3\"\tgcy\tr11\t\n\t0\t1\thax\t\t\"usetype,mag\"\tjew\n\n");
		const std::vector<Shown> shown = Summaries(book);
		CHECK(shown.size() == 2 && shown[0].title == "Amn Rune <- 3 Thul Runes + 1 Chipped Topaz" && shown[0].detail == "r11  <-  r10,qty=3 + gcy" && shown[1].title == "(no description)"
				&& shown[1].detail == "usetype,mag , jew  <-  hax   (not enabled)",
			"book entries: \"%s\" / \"%s\"", shown[0].detail.c_str(), shown[1].detail.c_str());
		CHECK(Search(shown, "").size() == 2 && Search(shown, "AMN rune") == std::vector<uint32_t> { 0 } && Search(shown, "hax jew") == std::vector<uint32_t> { 1 } && Search(shown, "amn hax").empty(), "search: every word, any case");
	}

	// the legend
	{
		LegendTables tables;
		tables.types      = { "ItemType\tCode\tEquiv1\tEquiv2\nArmor\tarmo\t\t\nAny Shield\tshld\tarmo\tseco\nVoodoo Heads\thead\tshld\tnecr\nSecond Hand\tseco\t\t\n" };
		tables.items      = { "name\ttype\tcode\nBuckler\tshie\tbuc\n", "name\tcode\ttype\nPreserved Head\tne1\thead\nBuckler again\tbuc\tshie\n" };
		tables.properties = { "code\t*Id\tstat1\t*Tooltip\nac\t0\tarmorclass\t+# Defense\nExpansion\t\t\t\nres-cold\t1\tcoldresist\t\n" };
		tables.stats      = { "Stat\t*ID\tSend Other\nstrength\t0\t\nruned\t370\t\n" };
		std::vector<Shown> lines, valueLines;
		const std::string  legend = Legend(tables, &lines, &valueLines);
		const size_t       codeCount = lines.size();
		lines.insert(lines.end(), valueLines.begin(), valueLines.end()); // looked up together below
		const auto         line   = [&](const std::string& code) { // the entry of a code
            for (const Shown& shown : lines) {
                if (shown.title.rfind(code + "  -  ", 0) == 0) {
                    return shown.title + " | " + shown.detail;
                }
            }
            return std::string("not found");
		};
		CHECK(line("rar") == "rar  -  rare (yellow name) | recipe word: quality" && line("shld") == "shld  -  Any Shield | item type; a kind of armo (Armor) and seco (Second Hand); includes: head"
				&& line("ne1") == "ne1  -  Preserved Head | item of type head (Voodoo Heads)" && line("res-cold") == "res-cold  -  (stat coldresist) | property code (the mod columns of a recipe)"
				&& Search(lines, "voodoo").size() == 2,
			"legend entries for the panel: %s", line("shld").c_str());
		CHECK(legend.find("  rar     rare (yellow name)") != std::string::npos && legend.find("  useitem   the very item of input 1") != std::string::npos, "legend: the recipe words are explained");
		CHECK(legend.find("  shld  Any Shield                    a kind of armo (Armor) and seco (Second Hand)\r\n        includes: head\r\n") != std::string::npos
				&& legend.find("  armo  Armor                         \r\n        includes: head, shld\r\n") != std::string::npos,
			"legend: item types with their name, what they are a kind of and what they include");
		CHECK(legend.find("  buc   Buckler                                 shie\r\n") != std::string::npos && legend.find("Buckler again") == std::string::npos
				&& legend.find("  ne1   Preserved Head                          head (Voodoo Heads)\r\n") != std::string::npos && legend.find("  buc ") < legend.find("  ne1 "),
			"legend: items by code with name and type, the first name found for a code");
		CHECK(legend.find("  ac                    +# Defense\r\n") != std::string::npos && legend.find("  res-cold              (stat coldresist)\r\n") != std::string::npos && legend.find("Expansion ") == std::string::npos,
			"legend: property codes");
		CHECK(legend.find("    op 15 16 17 18  the INPUT ITEM's stat param, as it is now,      > value   < value   not value   = value") != std::string::npos && legend.find("    mod 1 chance   percent chance") != std::string::npos
				&& legend.find("  370    runed\r\n") != std::string::npos && line("op 18") == "op 18  -  the input item's stat param, as it is now, must be value | op condition"
				&& line("param").find("first number of the op condition") != std::string::npos && line("mod 1 ... mod 5").find("a property the first output gets") != std::string::npos && line("b mod 1 ... b mod 5").find("output b") != std::string::npos && line("c lvl / c plvl / c ilvl").find("output c") != std::string::npos && line("plvl").find("percent of the player's level") != std::string::npos && legend.find("B MOD 1 - 5: the same for output b.") != std::string::npos
				&& line("stat 370") == "stat 370  -  runed | stat number (param of an op condition)",
			"legend: op, param, value and the mod columns are explained, with every op and the stat numbers (%s)", line("op 18").c_str());
		bool split = true;
		for (size_t i = 0; i < lines.size(); ++i) { // the columns, ops, values and stat numbers are the second tab; the words and codes the first
			const std::string& t     = lines[i].title;
			const bool         value = t.rfind("op", 0) == 0 || t.rfind("stat ", 0) == 0 || t.rfind("param", 0) == 0 || t.find(" = ") != std::string::npos || t.rfind("mod 1", 0) == 0 || t.rfind("b mod", 0) == 0;
			const bool         code  = t.rfind("rar  -", 0) == 0 || t.rfind("shld  -", 0) == 0 || t.rfind("ne1  -", 0) == 0 || t.rfind("res-cold", 0) == 0 || t.rfind("useitem", 0) == 0;
			split = split && !(value && i < codeCount) && !(code && i >= codeCount);
		}
		CHECK(split && line("class = war") == "class = war  -  only a Warlock can use the recipe | value of class" && line("min diff = 2").find("Hell only") != std::string::npos
				&& line("mod 1 chance = blank or 0").find("always given") != std::string::npos && line("value = 1 ... 7 (with op 2)").find("1 Sunday") != std::string::npos,
			"legend: two tabs - words and codes; columns with what their values mean");
		// a name made from the cells: the result first, with the mod's names for codes
		{
			LegendTables t;
			t.items = { "name\ttype\tcode\nThul Rune\trune\tr10\nAmn Rune\trune\tr11\nChipped Topaz\tgem0\tgcy\n" };
			t.types = { "ItemType\tCode\nAny Shield\tshld\nRing\tring\n" };
			const Names names = ReadNames(t);
			Cells       c;
			c.cell.resize(kColumnCount);
			const auto set = [&](const char* column, const char* value) { c.cell[static_cast<size_t>(ColumnIndex(column))] = value; };
			set("input 1", "\"r10,qty=3\"");
			set("input 2", "gcy");
			set("output", "r11");
			const std::string rune = GenericName(c, names);
			set("input 1", "\"shld,rar,sock=4\"");
			set("input 2", "zz9");
			set("output", "\"useitem,eth\"");
			set("output b", "\"ring,mag\"");
			CHECK(rune == "Amn Rune <- 3 Thul Rune + Chipped Topaz" && GenericName(c, names) == "Ethereal Rare Any Shield (4 sockets) + Magic Ring <- Rare Any Shield (4 sockets) + zz9" && GenericName(Cells {}, names) == "? <- ?",
				"a generic name: the result first, then what goes in (%s | %s)", rune.c_str(), GenericName(c, names).c_str());
		}
		CHECK(ResultFirst("Staff of Kings + Amulet of the Viper -> Horadric Staff") == "Horadric Staff <- Staff of Kings + Amulet of the Viper" && ResultFirst("Horadric Staff <- Staff of Kings") == "Horadric Staff <- Staff of Kings"
				&& ResultFirst("Ancients' Pledge - rare - shld - RalOrtTal") == "Ancients' Pledge - rare - shld - RalOrtTal" && ResultFirst("a -> b -> c") == "c <- a -> b" && ResultFirst(" -> x") == " -> x" && ResultFirst("").empty(),
			"a description is turned result first once (\"A + B -> C\" to \"C <- A + B\"); other descriptions stay");
		// a button picture resized: two frames of 40 x 20 with a gap, ends red, middle green / blue
		{
			std::string source(40 + 81 * 20 * 4, '\0');
			source.replace(0, 4, "SpA1");
			source[4] = 31; source[6] = 40; source[8] = 81; source[12] = 20; source[20] = 2;
			for (int y = 0; y < 20; ++y) {
				for (int x = 0; x < 81; ++x) {
					if (x == 40) { continue; } // the gap
					const int in = x > 40 ? x - 41 : x;
					char*     p  = &source[40 + (y * 81 + x) * 4];
					p[in < 12 || in >= 28 ? 0 : x > 40 ? 2 : 1] = static_cast<char>(255);
					p[3] = static_cast<char>(255);
				}
			}
			const std::string made = ButtonSprite(source, 100, 10);
			const auto        at   = [&](int frame, int x, int y, int c) { return static_cast<uint8_t>(made[40 + (y * 200 + frame * 100 + x) * 4 + c]); };
			CHECK(made.size() == 40 + 200 * 10 * 4 && made.substr(0, 4) == "SpA1" && made[6] == 100 && static_cast<uint8_t>(made[8]) == 200 && made[12] == 10 && made[20] == 2
					&& at(0, 0, 0, 0) == 255 && at(0, 99, 9, 0) == 255 && at(0, 50, 5, 1) == 255 && at(0, 50, 5, 0) == 0 && at(1, 50, 5, 2) == 255 && at(1, 2, 2, 0) == 255 && at(1, 50, 5, 3) == 255
					&& ButtonSprite("nonsense", 100, 10).empty() && ButtonSprite(source.substr(0, 500), 100, 10).empty(),
				"a button picture of the game is resized: every frame keeps its ends, the middle fills the width");
		}
		CHECK(Legend({}).find("(not available: no itemtypes.txt") != std::string::npos, "legend: says so when a table is missing");
	}

	const Sheet nothing = ReadSheet("");
	CHECK(!nothing.usable && nothing.lines.empty() && !nothing.header.complete && nothing.header.cell.size() == kColumnCount, "an empty file is a sheet with no columns and no lines");
	const Sheet fresh = ReadSheet(FullHeader() + "\n");
	CHECK(fresh.usable && fresh.header.known == static_cast<int>(kColumnCount) && fresh.header.description == 0, "the first line written into a new file has every column");

	// offsets of the wide record
	CHECK(Offset(kWide, 0x4b) == 0x4b && Offset(kWide, 0x5a) == 0x68 && Offset(kWide, 0xb3) == 0xc3 && Offset(kWide, 0x153) == 0x165, "offsets move the way D2RLoader moves them");
	CHECK(IsLineNumberByte(kWide, 0x5a) && IsLineNumberByte(kWide, 0x5a + 0x5b + 3) && !IsLineNumberByte(kWide, 0x5e) && !IsLineNumberByte(kWide, 0x59), "the stored line numbers are not compared");
}

int main(int argc, char** argv) {
	if (argc > 1) {
		StockCheck(argv[1], "");
		StockCheck(argv[1], "base/");
	}
	OverlayChecks();
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
