// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "cube_core.h"

#include <algorithm>

#include <cstring>

namespace cube {

// The game's cubemain columns: name, kind and place in the 0x158-byte record, in the order of
// the table its loader builds (image 0x6ab3782c, function 0x3e3ff0).
const Column kColumns[] = {
	{ "enabled", Kind::Byte, 0x0 },
	{ "firstLadderSeason", Kind::Word, 0x10 },
	{ "lastLadderSeason", Kind::Word, 0x12 },
	{ "min diff", Kind::Byte, 0x1 },
	{ "op", Kind::Byte, 0x3 },
	{ "param", Kind::Param, 0x4 },
	{ "value", Kind::Dword, 0x8 },
	{ "class", Kind::Class, 0x2 },
	{ "numinputs", Kind::Byte, 0xc },
	{ "version", Kind::Word, 0xe },
	{ "input 1", Kind::Input, 0x0 },
	{ "input 2", Kind::Input, 0x1 },
	{ "input 3", Kind::Input, 0x2 },
	{ "input 4", Kind::Input, 0x3 },
	{ "input 5", Kind::Input, 0x4 },
	{ "input 6", Kind::Input, 0x5 },
	{ "input 7", Kind::Input, 0x6 },
	{ "output", Kind::Output, 0x0 },
	{ "lvl", Kind::Byte, 0x5a },
	{ "plvl", Kind::Byte, 0x5b },
	{ "ilvl", Kind::Byte, 0x5c },
	{ "mod 1", Kind::Mod, 0x69 },
	{ "mod 1 chance", Kind::Byte, 0x73 },
	{ "mod 1 param", Kind::Word, 0x6d },
	{ "mod 1 min", Kind::Word, 0x6f },
	{ "mod 1 max", Kind::Word, 0x71 },
	{ "mod 2", Kind::Mod, 0x75 },
	{ "mod 2 chance", Kind::Byte, 0x7f },
	{ "mod 2 param", Kind::Word, 0x79 },
	{ "mod 2 min", Kind::Word, 0x7b },
	{ "mod 2 max", Kind::Word, 0x7d },
	{ "mod 3", Kind::Mod, 0x81 },
	{ "mod 3 chance", Kind::Byte, 0x8b },
	{ "mod 3 param", Kind::Word, 0x85 },
	{ "mod 3 min", Kind::Word, 0x87 },
	{ "mod 3 max", Kind::Word, 0x89 },
	{ "mod 4", Kind::Mod, 0x8d },
	{ "mod 4 chance", Kind::Byte, 0x97 },
	{ "mod 4 param", Kind::Word, 0x91 },
	{ "mod 4 min", Kind::Word, 0x93 },
	{ "mod 4 max", Kind::Word, 0x95 },
	{ "mod 5", Kind::Mod, 0x99 },
	{ "mod 5 chance", Kind::Byte, 0xa3 },
	{ "mod 5 param", Kind::Word, 0x9d },
	{ "mod 5 min", Kind::Word, 0x9f },
	{ "mod 5 max", Kind::Word, 0xa1 },
	{ "output b", Kind::Output, 0x1 },
	{ "b lvl", Kind::Byte, 0xb3 },
	{ "b plvl", Kind::Byte, 0xb4 },
	{ "b ilvl", Kind::Byte, 0xb5 },
	{ "b mod 1", Kind::Mod, 0xc2 },
	{ "b mod 1 chance", Kind::Byte, 0xcc },
	{ "b mod 1 param", Kind::Word, 0xc6 },
	{ "b mod 1 min", Kind::Word, 0xc8 },
	{ "b mod 1 max", Kind::Word, 0xca },
	{ "b mod 2", Kind::Mod, 0xce },
	{ "b mod 2 chance", Kind::Byte, 0xd8 },
	{ "b mod 2 param", Kind::Word, 0xd2 },
	{ "b mod 2 min", Kind::Word, 0xd4 },
	{ "b mod 2 max", Kind::Word, 0xd6 },
	{ "b mod 3", Kind::Mod, 0xda },
	{ "b mod 3 chance", Kind::Byte, 0xe4 },
	{ "b mod 3 param", Kind::Word, 0xde },
	{ "b mod 3 min", Kind::Word, 0xe0 },
	{ "b mod 3 max", Kind::Word, 0xe2 },
	{ "b mod 4", Kind::Mod, 0xe6 },
	{ "b mod 4 chance", Kind::Byte, 0xf0 },
	{ "b mod 4 param", Kind::Word, 0xea },
	{ "b mod 4 min", Kind::Word, 0xec },
	{ "b mod 4 max", Kind::Word, 0xee },
	{ "b mod 5", Kind::Mod, 0xf2 },
	{ "b mod 5 chance", Kind::Byte, 0xfc },
	{ "b mod 5 param", Kind::Word, 0xf6 },
	{ "b mod 5 min", Kind::Word, 0xf8 },
	{ "b mod 5 max", Kind::Word, 0xfa },
	{ "output c", Kind::Output, 0x2 },
	{ "c lvl", Kind::Byte, 0x10c },
	{ "c plvl", Kind::Byte, 0x10d },
	{ "c ilvl", Kind::Byte, 0x10e },
	{ "c mod 1", Kind::Mod, 0x11b },
	{ "c mod 1 chance", Kind::Byte, 0x125 },
	{ "c mod 1 param", Kind::Word, 0x11f },
	{ "c mod 1 min", Kind::Word, 0x121 },
	{ "c mod 1 max", Kind::Word, 0x123 },
	{ "c mod 2", Kind::Mod, 0x127 },
	{ "c mod 2 chance", Kind::Byte, 0x131 },
	{ "c mod 2 param", Kind::Word, 0x12b },
	{ "c mod 2 min", Kind::Word, 0x12d },
	{ "c mod 2 max", Kind::Word, 0x12f },
	{ "c mod 3", Kind::Mod, 0x133 },
	{ "c mod 3 chance", Kind::Byte, 0x13d },
	{ "c mod 3 param", Kind::Word, 0x137 },
	{ "c mod 3 min", Kind::Word, 0x139 },
	{ "c mod 3 max", Kind::Word, 0x13b },
	{ "c mod 4", Kind::Mod, 0x13f },
	{ "c mod 4 chance", Kind::Byte, 0x149 },
	{ "c mod 4 param", Kind::Word, 0x143 },
	{ "c mod 4 min", Kind::Word, 0x145 },
	{ "c mod 4 max", Kind::Word, 0x147 },
	{ "c mod 5", Kind::Mod, 0x14b },
	{ "c mod 5 chance", Kind::Byte, 0x155 },
	{ "c mod 5 param", Kind::Word, 0x14f },
	{ "c mod 5 min", Kind::Word, 0x151 },
	{ "c mod 5 max", Kind::Word, 0x153 },
};
const size_t kColumnCount = sizeof(kColumns) / sizeof(kColumns[0]);

namespace {

// The three output slots: where they start and how long one is, per layout.
constexpr size_t kStockOutputs = 0x4c, kStockOutput = 0x59;
constexpr size_t kWideOutputs  = 0x5a, kWideOutput  = 0x5b;

std::string_view Trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\r')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) {
		text.remove_suffix(1);
	}
	return text;
}

// Cell `index` of a tab-separated line ("" past the end).
std::string_view Cell(std::string_view line, int index) {
	if (index < 0) {
		return {};
	}
	size_t start = 0;
	for (int i = 0; i < index; ++i) {
		start = line.find('\t', start);
		if (start == std::string_view::npos) {
			return {};
		}
		++start;
	}
	const size_t end = line.find('\t', start);
	return Trim(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

// A line's cells, each trimmed, found in one pass (Cell() walks the line from its start every time).
using CellViews = std::vector<std::string_view>;
void SplitCells(std::string_view line, CellViews& cells) {
	cells.clear();
	size_t start = 0;
	for (;;) {
		const size_t end = line.find('\t', start);
		cells.push_back(Trim(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)));
		if (end == std::string_view::npos) {
			return;
		}
		start = end + 1;
	}
}
inline std::string_view At(const CellViews& cells, int index) {
	return index >= 0 && static_cast<size_t>(index) < cells.size() ? cells[static_cast<size_t>(index)] : std::string_view {};
}

// What a line's record depends on, whatever the file's columns are: its cells in the order of
// kColumns. Two lines with the same key compile to the same record, so one Cache serves every
// sheet (the description and columns the game does not read are left out).
void RecordKey(const Header& header, const CellViews& cells, std::string& key) {
	key.clear();
	for (size_t c = 0; c < kColumnCount; ++c) {
		key += At(cells, header.cell[c]);
		key += '\t';
	}
}

bool SameName(std::string_view a, const char* b) {
	const size_t length = strlen(b);
	if (a.size() != length) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}
	return true;
}

// A number the way the game's table reader takes one: optional sign, digits, the rest ignored.
int64_t Number(std::string_view text) {
	size_t i        = 0;
	bool   negative = false;
	if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
		negative = text[i] == '-';
		++i;
	}
	int64_t value = 0;
	for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
		value = value * 10 + (text[i] - '0');
		if (value > 0xffffffffll) {
			break;
		}
	}
	return negative ? -value : value;
}

void Put(uint8_t* at, uint64_t value, size_t bytes) {
	for (size_t i = 0; i < bytes; ++i) {
		at[i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

}

size_t Offset(const Layout& layout, size_t stockOffset) {
	if (!layout.wide || stockOffset < kStockOutputs) {
		return stockOffset;
	}
	// D2RLoader's LoadExcelTable: the inputs grew by 14 bytes, every output slot by 2
	const size_t inOutputs = stockOffset - kStockOutputs;
	return kWideOutputs + inOutputs / kStockOutput * kWideOutput + inOutputs % kStockOutput;
}

bool IsLineNumberByte(const Layout& layout, size_t offset) {
	const size_t first = layout.wide ? kWideOutputs : kStockOutputs;
	const size_t size  = layout.wide ? kWideOutput : kStockOutput;
	return offset >= first && offset < first + 3 * size && (offset - first) % size < 4;
}

bool SameRecipe(const Layout& layout, const uint8_t* a, const uint8_t* b) {
	for (size_t i = 0; i < layout.rowSize; ++i) {
		if (a[i] != b[i] && !IsLineNumberByte(layout, i)) {
			return false;
		}
	}
	return true;
}

Header ReadHeader(std::string_view line) {
	Header header;
	header.cell.assign(kColumnCount, -1);
	int index = 0;
	for (size_t start = 0; start <= line.size(); ++index) {
		const size_t           tab  = line.find('\t', start);
		const std::string_view name = Trim(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
		if (SameName(name, "description") && header.description < 0) {
			header.description = index;
		}
		if (SameName(name, "*action") && header.action < 0) {
			header.action = index;
		}
		header.complete = header.complete || SameName(name, "*complete");
		for (size_t c = 0; c < kColumnCount; ++c) {
			if (header.cell[c] < 0 && SameName(name, kColumns[c].name)) {
				header.cell[c] = index;
				++header.known;
				break;
			}
		}
		if (tab == std::string_view::npos) {
			break;
		}
		start = tab + 1;
	}
	return header;
}

bool IsBlank(std::string_view line) {
	for (const char c : line) {
		if (c != '\t' && c != ' ' && c != '\r') {
			return false;
		}
	}
	return true;
}

Sheet ReadSheet(std::string text) {
	Sheet sheet;
	sheet.text = std::move(text);
	std::string_view rest = sheet.text;
	if (rest.size() >= 3 && rest.substr(0, 3) == "\xef\xbb\xbf") {
		rest.remove_prefix(3);
	}
	sheet.header = ReadHeader({}); // an empty file: no column is there
	bool first   = true;
	while (!rest.empty()) {
		const size_t     end  = rest.find('\n');
		std::string_view line = rest.substr(0, end);
		rest                  = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (first) {
			sheet.headerLine = line;
			sheet.header     = ReadHeader(line);
			first            = false;
		} else {
			sheet.lines.push_back(line);
		}
	}
	const auto has = [&](const char* name) {
		for (size_t c = 0; c < kColumnCount; ++c) {
			if (strcmp(kColumns[c].name, name) == 0) {
				return sheet.header.cell[c] >= 0;
			}
		}
		return false;
	};
	sheet.usable = has("enabled") && has("numinputs") && has("input 1") && has("output");
	return sheet;
}

std::string Describe(const Sheet& sheet, size_t line) {
	std::string out = "line " + std::to_string(line + 2);
	if (line < sheet.lines.size()) {
		std::string_view about = Cell(sheet.lines[line], sheet.header.description);
		if (!about.empty()) {
			out += " (";
			out += about.size() > 48 ? std::string(about.substr(0, 45)) + "..." : std::string(about);
			out += ")";
		}
	}
	return out;
}

bool CompileLine(Game& game, const Layout& layout, const Header& header, std::string_view text, int line, uint8_t* row, std::string& problem, bool* sensitive) {
	memset(row, 0, layout.rowSize);
	if (sensitive != nullptr) {
		*sensitive = false;
	}
	// a blank number cell: 0, unless the byte is already set and the game leaves such bytes alone
	const auto blank = [&](size_t at, size_t bytes) {
		bool set = false;
		for (size_t i = 0; i < bytes; ++i) {
			set = set || row[at + i] != 0;
		}
		if (set && sensitive != nullptr) {
			*sensitive = true;
		}
		if (!layout.blankKeeps) {
			memset(row + at, 0, bytes);
		}
	};
	char      scratch[512];
	CellViews cells;
	cells.reserve(kColumnCount + 8);
	SplitCells(text, cells);
	for (size_t c = 0; c < kColumnCount; ++c) {
		const Column&          column = kColumns[c];
		const std::string_view cell   = At(cells, header.cell[c]);
		const size_t           at     = Offset(layout, column.at);
		if (cell.size() >= sizeof(scratch)) {
			problem = std::string(column.name) + ": the cell is too long";
			return false;
		}
		memcpy(scratch, cell.data(), cell.size());
		scratch[cell.size()] = '\0';
		scratch[cell.size() + 1 < sizeof(scratch) ? cell.size() + 1 : cell.size()] = '\0';
		switch (column.kind) {
		case Kind::Byte:
		case Kind::Word:
		case Kind::Dword: {
			const size_t bytes = column.kind == Kind::Byte ? 1 : column.kind == Kind::Word ? 2 : 4;
			if (cell.empty()) {
				blank(at, bytes);
			} else {
				Put(row + at, static_cast<uint64_t>(Number(cell)), bytes);
			}
			break;
		}
		case Kind::Param:
			if (cell.empty()) {
				blank(at, 4);
			} else if (cell[0] == '-' || (cell[0] >= '0' && cell[0] <= '9')) {
				Put(row + at, static_cast<uint64_t>(Number(cell)), 4);
			} else {
				Put(row + at, game.Param(scratch), 4);
			}
			break;
		case Kind::Class: {
			const int id = cell.empty() ? -1 : game.Class(scratch);
			if (id < 0 && !cell.empty()) {
				problem = "class: \"" + std::string(cell) + "\" is not a class code";
				return false;
			}
			row[at] = static_cast<uint8_t>(id);
			break;
		}
		case Kind::Mod:
			Put(row + at, 0xffffffffu, 4);
			if (!cell.empty() && !game.Mod(scratch, row, at)) {
				problem = std::string(column.name) + ": \"" + std::string(cell) + "\" is not a property code";
				return false;
			}
			break;
		case Kind::Input:
			if (!cell.empty() && !game.Input(scratch, row, column.at, line)) {
				problem = std::string(column.name) + ": the game does not know \"" + std::string(cell) + "\" (not an item code, an item type or a unique/set item name)";
				return false;
			}
			break;
		case Kind::Output:
			if (!cell.empty() && !game.Output(scratch, row, column.at, line)) {
				problem = std::string(column.name) + ": the game does not know \"" + std::string(cell) + "\" (not an item code, an item type, a unique/set item name or an output word)";
				return false;
			}
			break;
		}
	}
	return true;
}

namespace {

size_t IngredientsEnd(const Layout& layout) {
	return layout.wide ? kWideOutputs : kStockOutputs;
}

uint64_t IngredientsHash(const Layout& layout, const uint8_t* row) {
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 1; i < IngredientsEnd(layout); ++i) {
		hash = (hash ^ row[i]) * 1099511628211ull;
	}
	return hash;
}

}

bool SameIngredients(const Layout& layout, const uint8_t* a, const uint8_t* b) {
	return memcmp(a + 1, b + 1, IngredientsEnd(layout) - 1) == 0;
}

std::string FullHeader() {
	std::string out = "description";
	for (size_t c = 0; c < kColumnCount; ++c) {
		out += "\t";
		out += kColumns[c].name;
	}
	return out + "\t*action\t*eol";
}

Outcome Overlay(Game& game, const Layout& layout, const uint8_t* gameRows, size_t gameCount, const Sheet& mine, Cache& cache) {
	Outcome out;
	// compiled lines are only good for the header they were read under
	static const std::string   kHeaderKey = "\x01header";
	const std::vector<uint8_t> headerBytes(mine.headerLine.begin(), mine.headerLine.end());
	if (const auto held = cache.find(kHeaderKey); held == cache.end() || held->second != headerBytes) {
		cache.clear();
		cache[kHeaderKey] = headerBytes;
	}

	// the user's lines, grouped by ingredients (lines with the same ingredients stay together)
	struct Mine {
		const uint8_t* record;
		size_t         line;
		bool           remove;
	};
	struct Group {
		std::vector<Mine> rows; // in file order
		bool              placed = false;
	};
	std::vector<Group>                               groups;
	std::unordered_map<uint64_t, std::vector<size_t>> byHash;
	std::vector<uint8_t>                             row(layout.rowSize);
	std::string                                      problem;
	out.status.assign(mine.lines.size(), LineStatus::Blank);
	const auto groupOf = [&](const uint8_t* record) -> Group* {
		const auto bucket = byHash.find(IngredientsHash(layout, record));
		if (bucket != byHash.end()) {
			for (const size_t g : bucket->second) {
				if (SameIngredients(layout, groups[g].rows[0].record, record)) {
					return &groups[g];
				}
			}
		}
		return nullptr;
	};
	for (size_t i = 0; i < mine.lines.size(); ++i) {
		const std::string_view text = mine.lines[i];
		if (IsBlank(text)) {
			continue;
		}
		const std::string key(text);
		auto              held = cache.find(key);
		if (held == cache.end()) {
			const bool ok = mine.usable && CompileLine(game, layout, mine.header, text, static_cast<int>(i), row.data(), problem);
			held          = cache.emplace(key, ok ? row : std::vector<uint8_t> {}).first; // empty: refused
		}
		if (held->second.empty()) {
			++out.refused;
			out.status[i] = LineStatus::Refused;
			if (out.problems.size() < 20) {
				if (!mine.usable) {
					problem = "the file's first line lacks the columns a recipe needs";
				} else {
					CompileLine(game, layout, mine.header, text, static_cast<int>(i), row.data(), problem);
				}
				out.problems.push_back(Describe(mine, i) + ": " + problem);
			}
			continue;
		}
		const Mine entry { held->second.data(), i, SameName(Cell(text, mine.header.action), "remove") };
		if (Group* group = groupOf(entry.record)) {
			group->rows.push_back(entry);
		} else {
			byHash[IngredientsHash(layout, entry.record)].push_back(groups.size());
			groups.push_back({ { entry }, false });
		}
	}

	// the game's list: a recipe the user has a line for goes; the first one's place is the line's
	std::vector<const uint8_t*> list;
	list.reserve(gameCount + groups.size());
	const auto emit = [&](Group& group, bool inPlace) {
		for (const Mine& entry : group.rows) {
			if (entry.remove) {
				++(inPlace ? out.removed : out.idle);
				out.status[entry.line] = inPlace ? LineStatus::Removed : LineStatus::Idle;
				continue;
			}
			list.push_back(entry.record);
			++(inPlace ? out.replaced : out.added);
			out.disabled += entry.record[0] == 0 ? 1 : 0;
			out.status[entry.line] = inPlace ? LineStatus::Replaced : LineStatus::Added;
		}
		group.placed = true;
	};
	for (size_t r = 0; r < gameCount; ++r) {
		const uint8_t* record = gameRows + r * layout.rowSize;
		Group*         group  = groups.empty() ? nullptr : groupOf(record);
		if (group == nullptr) {
			list.push_back(record);
			continue;
		}
		++out.covered;
		if (!group->placed) {
			emit(*group, true);
		}
	}
	std::vector<const uint8_t*> front;
	front.swap(list);
	for (Group& group : groups) { // new recipes: in front, in the file's order
		if (!group.placed) {
			emit(group, false);
		}
	}
	list.insert(list.end(), front.begin(), front.end());

	out.count = list.size();
	out.rows.resize(out.count * layout.rowSize);
	for (size_t i = 0; i < list.size(); ++i) {
		memcpy(out.rows.data() + i * layout.rowSize, list[i], layout.rowSize);
	}
	return out;
}

Listing ListAll(Game& game, const Layout& layout, const uint8_t* rows, size_t count, const std::vector<const Sheet*>& sources, Cache& cache) {
	const auto hashOf = [&](const uint8_t* row) {
		uint64_t hash = 14695981039346656037ull;
		for (size_t i = 0; i < layout.rowSize; ++i) {
			hash = (hash ^ (IsLineNumberByte(layout, i) ? 0 : row[i])) * 1099511628211ull;
		}
		return hash;
	};
	struct Line {
		const Sheet*   sheet;
		size_t         line;
		const uint8_t* record;
	};
	std::unordered_map<uint64_t, std::vector<Line>> byHash;
	std::vector<uint8_t>                            row(layout.rowSize);
	std::string                                     problem, key;
	CellViews                                       cells;
	for (const Sheet* sheet : sources) {
		if (sheet == nullptr || !sheet->usable) {
			continue;
		}
		for (size_t i = 0; i < sheet->lines.size(); ++i) {
			const std::string_view text = sheet->lines[i];
			if (IsBlank(text)) {
				continue;
			}
			SplitCells(text, cells);
			RecordKey(sheet->header, cells, key);
			auto held = cache.find(key);
			if (held == cache.end()) {
				const bool ok = CompileLine(game, layout, sheet->header, text, static_cast<int>(i), row.data(), problem);
				held          = cache.emplace(key, ok ? row : std::vector<uint8_t> {}).first;
			}
			if (!held->second.empty()) {
				byHash[hashOf(held->second.data())].push_back({ sheet, i, held->second.data() });
			}
		}
	}
	Listing out;
	out.text = FullHeader() + "\r\n";
	out.text.reserve(count * 220);
	for (size_t r = 0; r < count; ++r) {
		const uint8_t* record = rows + r * layout.rowSize;
		const Line*    from   = nullptr;
		if (const auto bucket = byHash.find(hashOf(record)); bucket != byHash.end()) {
			for (const Line& line : bucket->second) {
				if (SameRecipe(layout, line.record, record)) {
					from = &line;
					break;
				}
			}
		}
		out.hasText.push_back(from != nullptr ? 1 : 0);
		if (from == nullptr) {
			++out.missing;
			out.mine.push_back(-1);
			out.text += "(recipe " + std::to_string(r + 1) + " of the cube's list: no recipe file has a line for it, so it cannot be shown as text)";
			out.text.append(kColumnCount, '\t');
			out.text += "\t\t0\r\n";
			continue;
		}
		++out.found;
		out.mine.push_back(!sources.empty() && from->sheet == sources[0] ? static_cast<int>(from->line) : -1);
		SplitCells(from->sheet->lines[from->line], cells);
		out.text += At(cells, from->sheet->header.description);
		for (size_t c = 0; c < kColumnCount; ++c) {
			out.text += '\t';
			out.text += At(cells, from->sheet->header.cell[c]);
		}
		out.text += "\t\t0\r\n";
	}
	return out;
}

Shown Summary(const Sheet& sheet, size_t at) {
	const auto lower = [](std::string text) {
		for (char& c : text) {
			c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}
		return text;
	};
	Shown     shown;
	CellViews cells;
	cells.reserve(kColumnCount + 8);
	SplitCells(sheet.lines[at], cells);
	shown.title = ResultFirst(At(cells, sheet.header.description));
	std::string inputs, outputs;
	for (size_t c = 0; c < kColumnCount; ++c) {
		if (kColumns[c].kind != Kind::Input && kColumns[c].kind != Kind::Output) {
			continue;
		}
		const std::string_view cell = At(cells, sheet.header.cell[c]);
		if (cell.empty()) {
			continue;
		}
		std::string& to = kColumns[c].kind == Kind::Input ? inputs : outputs;
		if (!to.empty()) {
			to += kColumns[c].kind == Kind::Input ? " + " : " , ";
		}
		for (const char letter : cell) { // "skl,qty=3" without its quotes
			if (letter != '"') {
				to += letter;
			}
		}
	}
	shown.detail = inputs.empty() && outputs.empty() ? std::string() : outputs + "  <-  " + inputs; // the result first
	if (At(cells, sheet.header.cell[0]) == "0") {
		shown.detail += "   (not enabled)";
	}
	if (shown.title.empty()) {
		shown.title = "(no description)";
	}
	shown.search = lower(shown.title + " " + shown.detail);
	return shown;
}

std::vector<Shown> Summaries(const Sheet& sheet) {
	std::vector<Shown> out;
	out.reserve(sheet.lines.size());
	for (size_t i = 0; i < sheet.lines.size(); ++i) {
		if (!IsBlank(sheet.lines[i])) {
			out.push_back(Summary(sheet, i));
		}
	}
	return out;
}

std::vector<uint32_t> Search(const std::vector<Shown>& all, std::string_view words) {
	std::vector<std::string> wanted;
	std::string              word;
	for (size_t i = 0; i <= words.size(); ++i) {
		const char c = i < words.size() ? words[i] : ' ';
		if (c == ' ' || c == '\t') {
			if (!word.empty()) {
				wanted.push_back(word);
			}
			word.clear();
		} else {
			word += c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}
	}
	std::vector<uint32_t> out;
	for (size_t i = 0; i < all.size(); ++i) {
		bool match = true;
		for (const std::string& w : wanted) {
			match = match && all[i].search.find(w) != std::string::npos;
		}
		if (match) {
			out.push_back(static_cast<uint32_t>(i));
		}
	}
	return out;
}

namespace {

const char* const kLegendWords =
	"LEGEND FOR CUBE RECIPES\r\n"
	"\r\n"
	"Written by Live Cube Recipes at every start of the game, from the tables the game has loaded.\r\n"
	"A recipe's description is free text by whoever wrote the recipe; what counts are its cells.\r\n"
	"In many descriptions the words between the dashes repeat the first input cell: for example\r\n"
	"\"Ancients' Pledge - rare - shld - RalOrtTal\" has input 1 = \"shld,rar\": a rare item of type shld.\r\n"
	"\r\n"
	"============================================================\r\n"
	"THE COLUMNS OF A RECIPE          (as Blizzard's own data guide for cubemain.txt describes them)\r\n"
	"============================================================\r\n"
	"  description   free text for people; the game does not read it\r\n"
	"  enabled       1 = the cube uses the recipe, 0 = it is switched off\r\n"
	"  min diff      the lowest difficulty it works in: 0 all, 1 Nightmare and Hell, 2 Hell only\r\n"
	"  version       0 = also for classic characters, 100 = expansion characters only\r\n"
	"  class         a class code (ama, sor, nec, pal, bar, dru, ass, war): only that class can use it\r\n"
	"  numinputs     how many items have to be in the cube, counting every one (\"r10,qty=3\" counts 3)\r\n"
	"  input 1 - 7   what has to be in the cube (words: list 1)\r\n"
	"  output, output b, output c   up to three things that come out (words: after list 1)\r\n"
	"\r\n"
	"  LVL, PLVL, ILVL: the item level the output is made with (it decides which properties the\r\n"
	"  item can get). They belong to the first output; b lvl, b plvl, b ilvl are the same for\r\n"
	"  output b, and c lvl, c plvl, c ilvl for output c.\r\n"
	"    lvl    a fixed item level. When it is filled in, plvl and ilvl are ignored\r\n"
	"    plvl   percent of the PLAYER's level that goes into the item level (100 = the player's level)\r\n"
	"    ilvl   percent of INPUT 1's item level that goes into the item level (100 = the same level)\r\n"
	"    plvl and ilvl are added together.\r\n"
	"\r\n"
	"  firstLadderSeason, lastLadderSeason   the ladder seasons the recipe is for; blank = always\r\n"
	"\r\n"
	"  OP, PARAM, VALUE: one extra condition the recipe has. op says which kind (a number from the\r\n"
	"  table below), param and value are the two numbers it works with. Blank op = no condition.\r\n"
	"  Where a stat is meant, param is the stat's number in ItemStatCost.txt (list 5).\r\n"
	"    op 1        the day of the month must be less than param or greater than value\r\n"
	"    op 2        the day of the week must not be value (1 Sunday ... 7 Saturday)\r\n"
	"    op 3 4 5 6      the PLAYER's stat param, as it is now, must be  > value   < value   not value   = value\r\n"
	"    op 7 8 9 10     the player's base stat param (without items)    > value   < value   not value   = value\r\n"
	"    op 11 12 13 14  the player's stat param from items and effects  > value   < value   not value   = value\r\n"
	"    op 15 16 17 18  the INPUT ITEM's stat param, as it is now,      > value   < value   not value   = value\r\n"
	"    op 19 20 21 22  the input item's base stat param                > value   < value   not value   = value\r\n"
	"    op 23 24 25 26  the input item's stat param beyond its base     > value   < value   not value   = value\r\n"
	"    op 27       the item's mod class (its unique number or quality) must not be value\r\n"
	"    op 28       for quest items: the item's quest difficulty must be above the game's difficulty\r\n"
	"  Example: op 18, param 370, value 0 = \"stat number 370 of the item put in must be 0\".\r\n"
	"\r\n"
	"  MOD 1 - 5: properties the first output gets, up to five.\r\n"
	"  B MOD 1 - 5: the same for output b.      C MOD 1 - 5: the same for output c.\r\n"
	"  (So a recipe with three outputs can give up to five properties to each of them.)\r\n"
	"  Each mod has five cells, here for mod 1; b mod 1 and c mod 1 have the same five:\r\n"
	"    mod 1          a property code (list 4), or a property group's code\r\n"
	"    mod 1 chance   percent chance that it is given; blank or 0 = always\r\n"
	"    mod 1 param    the property's parameter, when it needs one (a skill for a skill bonus, ...)\r\n"
	"    mod 1 min, mod 1 max   the amount: a number from min to max (the same = exactly that)\r\n"
	"  (The word mod inside an OUTPUT cell is something else: \"keep the properties of input 1\".)\r\n"
	"\r\n"
	"============================================================\r\n"
	"1. WORDS IN AN INPUT CELL        (\"shld,rar\"   \"r10,qty=3\"   \"jew\")\r\n"
	"============================================================\r\n"
	"The first word says WHAT goes into the cube:\r\n"
	"  an item code       that very item                    (r10 = Thul Rune; see list 3)\r\n"
	"  an item type code  any item of that type             (shld = any shield; see list 2)\r\n"
	"  a unique or set item's name   that unique or set item\r\n"
	"  any                any item at all\r\n"
	"The words after it, separated by commas, narrow it down:\r\n"
	"  qty=3   that many of it (3 here)\r\n"
	"  low     low quality (Cracked, Damaged, Low Quality, Crude)\r\n"
	"  nor     normal quality: a plain white (or grey) item, nothing magical\r\n"
	"  hiq     high quality: Superior\r\n"
	"  mag     magic (blue name)\r\n"
	"  rar     rare (yellow name)\r\n"
	"  set     a set item (green name)\r\n"
	"  uni     unique (gold name)\r\n"
	"  crf     crafted (orange name)\r\n"
	"  tmp     tempered\r\n"
	"  sock=2  with that many sockets          nos   with no sockets\r\n"
	"  eth     ethereal                        noe   not ethereal\r\n"
	"  bas     a normal-tier base item         exc   exceptional tier        eli   elite tier\r\n"
	"  upg     an item that allows upgrades\r\n"
	"  nru     not a rune word                 id    identified\r\n"
	"In descriptions the same things are often written out: normal, high quality, magic, rare, unique.\r\n"
	"\r\n"
	"============================================================\r\n"
	"WORDS IN AN OUTPUT CELL          (\"useitem\"   \"usetype,mag\"   \"rin,mag,pre=372\")\r\n"
	"============================================================\r\n"
	"The first word says WHAT comes out:\r\n"
	"  useitem   the very item of input 1, kept as it is (and changed by the words and mods after it)\r\n"
	"  usetype   a new item of the same kind as input 1\r\n"
	"  an item code, an item type code (a random item of that type), a unique or set item's name\r\n"
	"  Cow Portal, Pandemonium Portal, Pandemonium Finale Portal, Red Portal   a portal opens\r\n"
	"The words after it:\r\n"
	"  low nor hiq mag rar set uni crf tmp   the quality it gets (as above)\r\n"
	"  qty=3     that many                      lvl=50   a number for other words (Red Portal: the level it leads to)\r\n"
	"  pre=372   magic prefix number 372        suf=10   magic suffix number 10\r\n"
	"  sock=3    it gets that many sockets      eth      it is made ethereal\r\n"
	"  mod       it keeps the properties of input 1\r\n"
	"  uns       its sockets are emptied; what was in them is destroyed\r\n"
	"  rem       its sockets are emptied; what was in them is given back\r\n"
	"  reg       with usetype on a unique: the unique is made again (rerolled)\r\n"
	"  exc       it becomes its exceptional version      eli   its elite version\r\n"
	"  rep       it is repaired                          rch   it is recharged\r\n"
	"The columns mod 1 ... mod 5 add properties to the output (list 4), each with min and max.\r\n"
	"\r\n";

struct LegendTable {
	std::vector<std::string_view>              header;
	std::vector<std::vector<std::string_view>> rows;
	int column(const char* name) const {
		for (size_t i = 0; i < header.size(); ++i) {
			if (SameName(header[i], name)) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

LegendTable ReadLegendTable(const std::string& text) {
	LegendTable      table;
	std::string_view rest = text;
	bool             first = true;
	while (!rest.empty()) {
		const size_t     end  = rest.find('\n');
		std::string_view line = rest.substr(0, end);
		rest                  = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);
		std::vector<std::string_view> cells;
		for (size_t start = 0; start <= line.size();) {
			const size_t tab = line.find('\t', start);
			cells.push_back(Trim(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start)));
			if (tab == std::string_view::npos) {
				break;
			}
			start = tab + 1;
		}
		if (first) {
			table.header = cells;
			first        = false;
		} else if (!IsBlank(line)) {
			table.rows.push_back(cells);
		}
	}
	return table;
}

std::string Padded(std::string text, size_t width) {
	if (text.size() < width) {
		text.append(width - text.size(), ' ');
	}
	return text;
}

}

namespace {

// The recipe words for the legend panel (the file has them as running text).
struct Word {
	const char* word;
	const char* meaning;
	const char* where;
};
// The second tab: the columns, and what their values mean.
const Word kValues[] = {
	{ "enabled = 1", "the cube uses the recipe", "value of enabled" },
	{ "enabled = 0", "the recipe is switched off: the cube skips it", "value of enabled" },
	{ "min diff = 0 (or blank)", "works in every difficulty", "value of min diff" },
	{ "min diff = 1", "works in Nightmare and Hell only", "value of min diff" },
	{ "min diff = 2", "works in Hell only", "value of min diff" },
	{ "version = 0", "also for classic characters", "value of version" },
	{ "version = 100", "expansion characters only", "value of version" },
	{ "class = ama", "only an Amazon can use the recipe", "value of class (blank = every class)" },
	{ "class = sor", "only a Sorceress can use the recipe", "value of class" },
	{ "class = nec", "only a Necromancer can use the recipe", "value of class" },
	{ "class = pal", "only a Paladin can use the recipe", "value of class" },
	{ "class = bar", "only a Barbarian can use the recipe", "value of class" },
	{ "class = dru", "only a Druid can use the recipe", "value of class" },
	{ "class = ass", "only an Assassin can use the recipe", "value of class" },
	{ "class = war", "only a Warlock can use the recipe", "value of class" },
	{ "numinputs = 4", "four items have to be in the cube in all (r10,qty=3 counts as 3)", "value of numinputs" },
	{ "op = blank", "no extra condition", "value of op" },
	{ "param = a number (with op 3 - 26)", "the stat that is looked at: its number in ItemStatCost.txt (search: stat 370)", "value of param" },
	{ "param = a number (with op 1)", "a day of the month", "value of param" },
	{ "value = a number (with op 3 - 26)", "what the stat is compared with", "value of value" },
	{ "value = 1 ... 7 (with op 2)", "a day of the week: 1 Sunday, 2 Monday, 3 Tuesday, 4 Wednesday, 5 Thursday, 6 Friday, 7 Saturday", "value of value" },
	{ "lvl = 50", "the output is made with item level 50, whatever plvl and ilvl say", "value of lvl / b lvl / c lvl" },
	{ "plvl = 100", "the player's whole level goes into the output's item level (50 = half of it)", "value of plvl / b plvl / c plvl" },
	{ "ilvl = 100", "input 1's whole item level goes into the output's item level (50 = half of it)", "value of ilvl / b ilvl / c ilvl" },
	{ "mod 1 = a property code", "the property the output gets (the Codes tab lists them: search res-cold)", "value of mod 1-5, b mod 1-5, c mod 1-5" },
	{ "mod 1 chance = blank or 0", "the property is always given", "value of a mod's chance" },
	{ "mod 1 chance = 25", "the property is given 25 times in 100", "value of a mod's chance" },
	{ "mod 1 param = a number", "the property's parameter when it needs one (which skill, how many charges ...); blank otherwise", "value of a mod's param" },
	{ "mod 1 min = mod 1 max", "exactly that amount", "values of a mod's min and max" },
	{ "mod 1 min < mod 1 max", "a random amount from min to max", "values of a mod's min and max" },
	{ "firstLadderSeason / lastLadderSeason = blank", "the recipe is always there (a number: only in those ladder seasons)", "values of the ladder season columns" },
};
const Word kColumnWords[] = {
	{ "op", "one extra condition of the recipe: a number 1-28 saying which kind; blank = none", "recipe column (works with param and value)" },
	{ "param", "first number of the op condition: mostly the stat's number in ItemStatCost.txt", "recipe column (works with op)" },
	{ "value", "second number of the op condition: what the stat (or day) is compared with", "recipe column (works with op)" },
	{ "op 1", "the day of the month must be less than param or greater than value", "op condition" },
	{ "op 2", "the day of the week must not be value (1 Sunday ... 7 Saturday)", "op condition" },
	{ "op 3", "the player's stat param, as it is now, must be greater than value", "op condition" },
	{ "op 4", "the player's stat param, as it is now, must be less than value", "op condition" },
	{ "op 5", "the player's stat param, as it is now, must not be value", "op condition" },
	{ "op 6", "the player's stat param, as it is now, must be value", "op condition" },
	{ "op 7", "the player's base stat param must be greater than value", "op condition" },
	{ "op 8", "the player's base stat param must be less than value", "op condition" },
	{ "op 9", "the player's base stat param must not be value", "op condition" },
	{ "op 10", "the player's base stat param must be value", "op condition" },
	{ "op 11", "the player's stat param from items and effects must be greater than value", "op condition" },
	{ "op 12", "the player's stat param from items and effects must be less than value", "op condition" },
	{ "op 13", "the player's stat param from items and effects must not be value", "op condition" },
	{ "op 14", "the player's stat param from items and effects must be value", "op condition" },
	{ "op 15", "the input item's stat param, as it is now, must be greater than value", "op condition" },
	{ "op 16", "the input item's stat param, as it is now, must be less than value", "op condition" },
	{ "op 17", "the input item's stat param, as it is now, must not be value", "op condition" },
	{ "op 18", "the input item's stat param, as it is now, must be value", "op condition" },
	{ "op 19", "the input item's base stat param must be greater than value", "op condition" },
	{ "op 20", "the input item's base stat param must be less than value", "op condition" },
	{ "op 21", "the input item's base stat param must not be value", "op condition" },
	{ "op 22", "the input item's base stat param must be value", "op condition" },
	{ "op 23", "the input item's stat param beyond its base must be greater than value", "op condition" },
	{ "op 24", "the input item's stat param beyond its base must be less than value", "op condition" },
	{ "op 25", "the input item's stat param beyond its base must not be value", "op condition" },
	{ "op 26", "the input item's stat param beyond its base must be value", "op condition" },
	{ "op 27", "the item's mod class (its unique number or quality) must not be value", "op condition" },
	{ "op 28", "for quest items: the item's quest difficulty must be above the game's difficulty", "op condition" },
	{ "mod 1 ... mod 5", "a property the first output gets: a property code (or property group code)", "recipe columns (b mod and c mod are the same for outputs b and c)" },
	{ "mod 1 chance", "percent chance that the property is given; blank or 0 = always", "recipe column" },
	{ "mod 1 param", "the property's parameter, when it needs one (a skill for a skill bonus, ...)", "recipe column" },
	{ "mod 1 min / mod 1 max", "the amount: a number from min to max (the same = exactly that)", "recipe columns" },
	{ "enabled", "1 = the cube uses the recipe, 0 = it is switched off", "recipe column" },
	{ "min diff", "lowest difficulty it works in: 0 all, 1 Nightmare and Hell, 2 Hell only", "recipe column" },
	{ "version", "0 = also for classic characters, 100 = expansion characters only", "recipe column" },
	{ "class", "a class code (ama sor nec pal bar dru ass war): only that class can use it", "recipe column" },
	{ "numinputs", "how many items have to be in the cube, counting every one (r10,qty=3 counts 3)", "recipe column" },
	{ "lvl", "the output's item level as a fixed number (plvl and ilvl are then ignored)", "recipe column, for the first output" },
	{ "plvl", "percent of the player's level that goes into the output's item level (100 = the player's level)", "recipe column, for the first output; added to ilvl" },
	{ "ilvl", "percent of input 1's item level that goes into the output's item level (100 = the same)", "recipe column, for the first output; added to plvl" },
	{ "b lvl / b plvl / b ilvl", "lvl, plvl and ilvl for output b", "recipe columns" },
	{ "c lvl / c plvl / c ilvl", "lvl, plvl and ilvl for output c", "recipe columns" },
	{ "b mod 1 ... b mod 5", "a property output b gets: a property code (or property group code)", "recipe columns; each with b mod 1 chance, param, min, max" },
	{ "c mod 1 ... c mod 5", "a property output c gets: a property code (or property group code)", "recipe columns; each with c mod 1 chance, param, min, max" },
	{ "output / output b / output c", "up to three things that come out of one recipe, each with its own lvl cells and five mods", "recipe columns" },
};
const Word kWords[] = {
	{ "nor", "normal quality: a plain white (or grey) item, nothing magical", "recipe word: quality" },
	{ "hiq", "high quality: Superior", "recipe word: quality" },
	{ "low", "low quality (Cracked, Damaged, Low Quality, Crude)", "recipe word: quality" },
	{ "mag", "magic (blue name)", "recipe word: quality" },
	{ "rar", "rare (yellow name)", "recipe word: quality" },
	{ "set", "a set item (green name)", "recipe word: quality" },
	{ "uni", "unique (gold name)", "recipe word: quality" },
	{ "crf", "crafted (orange name)", "recipe word: quality" },
	{ "tmp", "tempered", "recipe word: quality" },
	{ "normal / high quality / magic / rare / unique", "in descriptions: the quality the input item must have", "words in recipe descriptions" },
	{ "qty=3", "that many of it (3 here)", "recipe word: input and output" },
	{ "sock=2", "input: with that many sockets; output: it gets that many", "recipe word: input and output" },
	{ "nos", "with no sockets", "recipe word: input" },
	{ "eth", "input: ethereal; output: it is made ethereal", "recipe word: input and output" },
	{ "noe", "not ethereal", "recipe word: input" },
	{ "bas", "a normal-tier base item", "recipe word: input" },
	{ "exc", "input: exceptional tier; output: it becomes its exceptional version", "recipe word: input and output" },
	{ "eli", "input: elite tier; output: it becomes its elite version", "recipe word: input and output" },
	{ "upg", "an item that allows upgrades", "recipe word: input" },
	{ "id", "identified", "recipe word: input" },
	{ "nru", "not a rune word", "recipe word: input" },
	{ "any", "any item at all", "recipe word: input" },
	{ "useitem", "the very item of input 1, kept as it is (and changed by the words and mods after it)", "recipe word: output" },
	{ "usetype", "a new item of the same kind as input 1", "recipe word: output" },
	{ "lvl=50", "a number for other output words (Red Portal: the level it leads to)", "recipe word: output" },
	{ "pre=372", "magic prefix number 372", "recipe word: output" },
	{ "suf=10", "magic suffix number 10", "recipe word: output" },
	{ "mod", "it keeps the properties of input 1", "recipe word: output" },
	{ "uns", "its sockets are emptied; what was in them is destroyed", "recipe word: output" },
	{ "rem", "its sockets are emptied; what was in them is given back", "recipe word: output" },
	{ "reg", "with usetype on a unique: the unique is made again (rerolled)", "recipe word: output" },
	{ "rep", "it is repaired", "recipe word: output" },
	{ "rch", "it is recharged", "recipe word: output" },
	{ "Cow Portal / Pandemonium Portal / Pandemonium Finale Portal / Red Portal", "a portal opens", "recipe word: output" },
};

}

std::string Legend(const LegendTables& tables, std::vector<Shown>* codes, std::vector<Shown>* values) {
	std::string out   = kLegendWords;
	const auto  entry = [&](const std::string& code, const std::string& meaning, const std::string& detail, bool value = false) {
		std::vector<Shown>* entries = value ? values : codes;
		if (entries == nullptr) {
			return;
		}
		Shown shown;
		shown.title   = meaning.empty() ? code : code + "  -  " + meaning;
		shown.detail  = detail;
		shown.code    = code;
		shown.meaning = meaning;
		if (!value) { // a word or code: itself, unless the entry names several ("a / b")
			shown.insert = code.find(" / ") == std::string::npos ? code : std::string();
		} else { // a column's explanation: only "op 18" and "stat 370" stand for something to type, the number
			const size_t space = code.find(' ');
			if (space != std::string::npos && space + 1 < code.size() && (code.compare(0, space, "op") == 0 || code.compare(0, space, "stat") == 0)
				&& code.find_first_not_of("0123456789", space + 1) == std::string::npos) {
				shown.insert = code.substr(space + 1);
			}
		}
		shown.search = shown.title + " " + shown.detail;
		for (char& c : shown.search) {
			c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}
		entries->push_back(std::move(shown));
	};
	for (const Word& word : kWords) {
		entry(word.word, word.meaning, word.where);
	}
	for (const Word& word : kColumnWords) {
		entry(word.word, word.meaning, word.where, true);
	}
	for (const Word& word : kValues) {
		entry(word.word, word.meaning, word.where, true);
	}
	const auto  title = [&](const char* text) { out += std::string("============================================================\r\n") + text + "\r\n============================================================\r\n"; };
	const auto  cell  = [](const std::vector<std::string_view>& row, int at) { return at >= 0 && static_cast<size_t>(at) < row.size() ? std::string(row[static_cast<size_t>(at)]) : std::string(); };

	// 2. item types: code -> name, parents; and what each includes
	struct Type {
		std::string code, name, parents[2];
	};
	std::vector<Type> types;
	const auto        typeAt = [&](const std::string& code) -> const Type* {
        for (const Type& type : types) {
            if (type.code == code) {
                return &type;
            }
        }
        return nullptr;
	};
	for (const std::string& text : tables.types) {
		const LegendTable table = ReadLegendTable(text);
		const int         code = table.column("Code"), name = table.column("ItemType"), first = table.column("Equiv1"), second = table.column("Equiv2");
		for (const auto& row : table.rows) {
			if (!cell(row, code).empty() && typeAt(cell(row, code)) == nullptr) {
				types.push_back({ cell(row, code), cell(row, name), { cell(row, first), cell(row, second) } });
			}
		}
	}
	title("2. ITEM TYPE CODES              (a type also stands for every type listed under \"includes\")");
	if (types.empty()) {
		out += "(not available: no itemtypes.txt of the mod could be read)\r\n";
	}
	std::vector<const Type*> sorted;
	for (const Type& type : types) {
		sorted.push_back(&type);
	}
	for (size_t i = 1; i < sorted.size(); ++i) { // by code
		for (size_t j = i; j > 0 && sorted[j]->code < sorted[j - 1]->code; --j) {
			const Type* held = sorted[j];
			sorted[j]        = sorted[j - 1];
			sorted[j - 1]    = held;
		}
	}
	const auto isKindOf = [&](const Type& type, const std::string& ancestor) {
		std::vector<const Type*> todo { &type };
		for (size_t done = 0; done < todo.size() && todo.size() < 64; ++done) {
			for (const std::string& parent : todo[done]->parents) {
				if (parent == ancestor) {
					return true;
				}
				if (const Type* up = parent.empty() ? nullptr : typeAt(parent)) {
					todo.push_back(up);
				}
			}
		}
		return false;
	};
	for (const Type* type : sorted) {
		out += "  " + Padded(type->code, 6) + Padded(type->name, 30);
		std::string kind;
		for (const std::string& parent : type->parents) {
			if (!parent.empty()) {
				const Type* up = typeAt(parent);
				kind += (kind.empty() ? "" : " and ") + parent + (up != nullptr ? " (" + up->name + ")" : "");
			}
		}
		out += kind.empty() ? "" : "a kind of " + kind;
		std::string includes;
		for (const Type* other : sorted) {
			if (other != type && isKindOf(*other, type->code)) {
				includes += (includes.empty() ? "" : ", ") + other->code;
			}
		}
		out += "\r\n";
		if (!includes.empty()) {
			out += "        includes: " + includes + "\r\n";
		}
		entry(type->code, type->name, "item type" + (kind.empty() ? std::string() : "; a kind of " + kind) + (includes.empty() ? std::string() : "; includes: " + includes));
	}
	out += "\r\n";

	// 3. items: code -> name, type
	struct Item {
		std::string code, name, type;
	};
	std::vector<Item>                        items;
	std::unordered_map<std::string, size_t> seen;
	for (const std::string& text : tables.items) {
		const LegendTable table = ReadLegendTable(text);
		const int         code = table.column("code"), name = table.column("name"), type = table.column("type");
		for (const auto& row : table.rows) {
			if (!cell(row, code).empty() && seen.emplace(cell(row, code), items.size()).second) {
				items.push_back({ cell(row, code), cell(row, name), cell(row, type) });
			}
		}
	}
	title("3. ITEM CODES                    (code, name, its item type)");
	if (items.empty()) {
		out += "(not available: no weapons.txt, armor.txt or misc.txt of the mod could be read)\r\n";
	}
	std::vector<const Item*> byCode;
	for (const Item& item : items) {
		byCode.push_back(&item);
	}
	for (size_t gap = byCode.size() / 2; gap > 0; gap /= 2) { // by code
		for (size_t i = gap; i < byCode.size(); ++i) {
			for (size_t j = i; j >= gap && byCode[j]->code < byCode[j - gap]->code; j -= gap) {
				const Item* held = byCode[j];
				byCode[j]        = byCode[j - gap];
				byCode[j - gap]  = held;
			}
		}
	}
	for (const Item* item : byCode) {
		const Type* type = typeAt(item->type);
		out += "  " + Padded(item->code, 6) + Padded(item->name, 40) + item->type + (type != nullptr ? " (" + type->name + ")" : "") + "\r\n";
		entry(item->code, item->name, "item of type " + item->type + (type != nullptr ? " (" + type->name + ")" : ""));
	}
	out += "\r\n";

	// 4. properties
	title("4. PROPERTY CODES                (the mod columns; code, what it does where the table says)");
	std::unordered_map<std::string, size_t> known;
	size_t                                  listed = 0;
	for (const std::string& text : tables.properties) {
		const LegendTable table = ReadLegendTable(text);
		const int         code = table.column("code"), tip = table.column("*Tooltip"), stat = table.column("stat1");
		for (const auto& row : table.rows) {
			if (cell(row, code).empty() || cell(row, code) == "Expansion" || !known.emplace(cell(row, code), listed).second) {
				continue;
			}
			++listed;
			const std::string about = !cell(row, tip).empty() ? cell(row, tip) : !cell(row, stat).empty() ? "(stat " + cell(row, stat) + ")" : "";
			out += "  " + Padded(cell(row, code), 22) + about + "\r\n";
			entry(cell(row, code), about, "property code (the mod columns of a recipe)");
		}
	}
	if (listed == 0) {
		out += "(not available: no properties.txt of the mod could be read)\r\n";
	}
	out += "\r\n";

	// 5. stat numbers, for op's param
	title("5. STAT NUMBERS                  (the number param means in an op condition; number, name)");
	std::unordered_map<std::string, size_t> stats;
	for (const std::string& text : tables.stats) {
		const LegendTable table = ReadLegendTable(text);
		const int         name = table.column("Stat"), id = table.column("*ID");
		size_t            index = 0;
		for (const auto& row : table.rows) {
			const std::string number = !cell(row, id).empty() ? cell(row, id) : std::to_string(index);
			++index;
			if (cell(row, name).empty() || !stats.emplace(number, index).second) {
				continue;
			}
			out += "  " + Padded(number, 7) + cell(row, name) + "\r\n";
			entry("stat " + number, cell(row, name), "stat number (param of an op condition)", true);
		}
	}
	if (stats.empty()) {
		out += "(not available: no itemstatcost.txt of the mod could be read)\r\n";
	}
	return out;
}

std::vector<HowItWorks> LegendHow(int section) {
	if (section == 0) {
		return {
			{ "Reading a recipe", "A recipe's description is free text by whoever wrote it; what counts are its cells. In many descriptions the words between the dashes repeat the first input cell: \"Ancients' Pledge - rare - shld - RalOrtTal\" has input 1 = \"shld,rar\", a rare item of type shld." },
			{ "Words in an input cell", "The first word says what goes into the cube: an item code (r10 = that very item), an item type code (shld = any shield), a unique or set item's name, or any. The words after it, separated by commas, narrow it down: qty=3, a quality (low nor hiq mag rar set uni crf tmp), sock=2 or nos, eth or noe, bas exc eli, upg, nru, id." },
			{ "Words in an output cell", "The first word says what comes out: useitem (input 1 itself, kept and changed), usetype (a new item of input 1's kind), an item or item type code, a unique or set item's name, or a portal (Cow Portal, Pandemonium Portal, Pandemonium Finale Portal, Red Portal). The words after it give its quality, qty, pre= / suf= (a magic prefix or suffix number), sock=, eth, mod (keeps input 1's properties), uns / rem (empties sockets), reg, exc / eli, rep, rch." },
			{ "Item types include others", "An item type code also stands for every type listed under \"includes\" in its entry: armo includes shld and head, so an input of armo takes a shield too." },
			{ "Putting a code into a cell", "While a recipe is open in the editor, a click on an entry puts its code into the cell being typed into: in an input or output cell after a comma (inside the quotes when there are quotes), in any other cell in place of what is there." },
		};
	}
	return {
		{ "The columns of a recipe", "enabled (1 on, 0 off), min diff (0 all, 1 Nightmare and Hell, 2 Hell), version (0 also classic, 100 expansion only), class (only that class), numinputs (every item counted: r10,qty=3 counts 3), input 1 - 7 (what goes in), output, output b, output c (up to three things come out), firstLadderSeason / lastLadderSeason (blank = always). As Blizzard's data guide for cubemain.txt describes them." },
		{ "Item level: lvl, plvl, ilvl", "The item level the first output is made with (it decides which properties it can get). lvl is a fixed level; when it is filled in, plvl and ilvl are ignored. plvl is percent of the player's level, ilvl percent of input 1's item level; the two are added. b lvl / b plvl / b ilvl are the same for output b, c lvl ... for output c." },
		{ "Conditions: op, param, value", "One extra condition: op says which kind (op 1 - 28), param and value are its two numbers; blank op = none. Where a stat is meant, param is the stat's number in ItemStatCost.txt (the stat entries). Example: op 18, param 370, value 0 = stat 370 of the item put in must be 0." },
		{ "Properties: mod 1 - 5", "Up to five properties for the first output (b mod 1 - 5 for output b, c mod 1 - 5 for output c). Each has five cells: mod 1 (a property code or property group code), mod 1 chance (percent; blank or 0 = always), mod 1 param (when the property needs one, such as a skill), mod 1 min and mod 1 max (the amount; the same = exactly that). The word mod inside an output cell is something else: keep input 1's properties." },
		{ "Putting a number into a cell", "A click on an op entry or a stat entry puts its number into the cell being typed into (op 18 puts 18, stat 370 puts 370). The other entries here explain a column and put nothing in." },
	};
}

int ColumnIndex(const char* name) {
	for (size_t c = 0; c < kColumnCount; ++c) {
		if (SameName(name, kColumns[c].name)) {
			return static_cast<int>(c);
		}
	}
	return -1;
}

Cells CellsOf(const Sheet& sheet, size_t line) {
	Cells cells;
	cells.cell.resize(kColumnCount);
	if (line >= sheet.lines.size()) {
		return cells;
	}
	CellViews views;
	views.reserve(kColumnCount + 8);
	SplitCells(sheet.lines[line], views);
	cells.description = std::string(At(views, sheet.header.description));
	for (size_t c = 0; c < kColumnCount; ++c) {
		cells.cell[c] = std::string(At(views, sheet.header.cell[c]));
	}
	cells.remove = SameName(At(views, sheet.header.action), "remove");
	return cells;
}

std::string LineOf(const Cells& cells) {
	const auto clean = [](std::string text) { // a cell cannot hold a tab or a line end
		for (char& c : text) {
			c = c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
		}
		return text;
	};
	std::string out = clean(cells.description);
	for (size_t c = 0; c < kColumnCount; ++c) {
		out += '\t';
		out += c < cells.cell.size() ? clean(cells.cell[c]) : std::string();
	}
	return out + (cells.remove ? "\tremove\t0" : "\t\t0");
}

bool SameIngredientCells(const Cells& a, const Cells& b) {
	for (size_t c = 0; c < kColumnCount && c < a.cell.size() && c < b.cell.size(); ++c) {
		if (kColumns[c].kind == Kind::Output) {
			break; // the outputs and what follows them are not ingredients
		}
		if (strcmp(kColumns[c].name, "enabled") != 0 && a.cell[c] != b.cell[c]) {
			return false;
		}
	}
	return true;
}

int CountInputs(const Cells& cells) {
	int total = 0;
	for (size_t c = 0; c < kColumnCount && c < cells.cell.size(); ++c) {
		if (kColumns[c].kind != Kind::Input || cells.cell[c].empty()) {
			continue;
		}
		const size_t at = cells.cell[c].find("qty=");
		const int    n  = at == std::string::npos ? 1 : static_cast<int>(Number(std::string_view(cells.cell[c]).substr(at + 4)));
		total += n > 0 ? n : 1;
	}
	return total;
}

std::string CompleteHeader() {
	return FullHeader() + "\t*complete";
}

std::vector<std::string> AmountNotes(const Header& header, std::string_view line) {
	std::vector<std::string> notes;
	CellViews                cells;
	SplitCells(line, cells);
	for (size_t c = 0; c + 4 < kColumnCount; ++c) {
		if (kColumns[c].kind != Kind::Mod) {
			continue;
		}
		// kColumns: "x mod N", then its chance, param, min and max
		const std::string_view code = At(cells, header.cell[c]);
		if (code.empty() || !At(cells, header.cell[c + 2]).empty() || !At(cells, header.cell[c + 3]).empty() || !At(cells, header.cell[c + 4]).empty()) {
			continue;
		}
		const std::string name = kColumns[c].name;
		notes.push_back(name + " " + std::string(code) + " has no amount: its min, max and param are blank (0), so the item gets nothing. Put the amount in " + name + " min and " + name + " max");
	}
	return notes;
}

namespace {

bool SameText(std::string_view a, std::string_view b) { // any case, as the game's name tables (A-Z only)
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}
	return true;
}

bool NamesIn(const Column& column, bool stats) {
	return stats ? column.kind == Kind::Param : column.kind == Kind::Mod;
}

}

int RenameInText(std::string& text, bool stats, std::string_view oldName, std::string_view newName, std::vector<std::string>* where) {
	if (oldName.empty() || newName.empty()) {
		return 0;
	}
	const size_t firstEnd = text.find('\n');
	if (firstEnd == std::string::npos) {
		return 0;
	}
	const Header header = ReadHeader(std::string_view(text).substr(0, firstEnd));
	std::vector<int> targets; // cells of the columns that may name it, with their kColumns index
	std::vector<size_t> columnOf;
	for (size_t c = 0; c < kColumnCount; ++c) {
		if (NamesIn(kColumns[c], stats) && header.cell[c] >= 0) {
			targets.push_back(header.cell[c]);
			columnOf.push_back(c);
		}
	}
	std::string out = text.substr(0, firstEnd + 1);
	int         changed = 0;
	size_t      line    = 0;
	for (size_t start = firstEnd + 1; start < text.size(); ++line) {
		size_t end = text.find('\n', start);
		end        = end == std::string::npos ? text.size() : end + 1;
		const std::string_view whole(text.data() + start, end - start);
		std::string_view       body = whole;
		std::string_view       tail;
		while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
			body.remove_suffix(1);
		}
		tail = whole.substr(body.size());
		std::string rebuilt;
		int         cell = 0;
		bool        any  = false;
		for (size_t from = 0;; ++cell) {
			const size_t           tab   = body.find('\t', from);
			const std::string_view piece = body.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from);
			bool                   hit   = false;
			for (size_t t = 0; t < targets.size() && !hit; ++t) {
				if (targets[t] == cell && SameText(Trim(piece), oldName)) {
					hit = true;
					if (where != nullptr) {
						const std::string_view about = Cell(body, header.description);
						where->push_back("line " + std::to_string(line + 2) + (about.empty() ? std::string() : " (" + std::string(about.substr(0, 45)) + ")") + " " + kColumns[columnOf[t]].name);
					}
				}
			}
			rebuilt += cell == 0 ? "" : "\t";
			rebuilt += hit ? std::string(newName) : std::string(piece);
			changed += hit ? 1 : 0;
			any = any || hit;
			if (tab == std::string_view::npos) {
				break;
			}
			from = tab + 1;
		}
		out += any ? rebuilt + std::string(tail) : std::string(whole);
		start = end;
	}
	if (changed != 0) {
		text.swap(out);
	}
	return changed;
}

int RenameInCells(Cells& cells, bool stats, std::string_view oldName, std::string_view newName) {
	int changed = 0;
	for (size_t c = 0; c < kColumnCount && c < cells.cell.size(); ++c) {
		if (NamesIn(kColumns[c], stats) && !oldName.empty() && !newName.empty() && SameText(Trim(cells.cell[c]), oldName)) {
			cells.cell[c] = std::string(newName);
			++changed;
		}
	}
	return changed;
}

int NamesRow(const Layout& layout, const uint8_t* row, bool stats, uint32_t from) {
	for (size_t c = 0; c < kColumnCount; ++c) {
		const Column& column = kColumns[c];
		const size_t  at     = Offset(layout, column.at);
		if (stats && column.kind == Kind::Param) {
			uint32_t param = 0;
			memcpy(&param, row + at, sizeof(param));
			const uint8_t op = row[Offset(layout, 0x3)];
			if (op >= 3 && op <= 26 && param >= from && param != 0xffffffffu) { // ops 3-26: param is a stat (the legend's op list)
				return static_cast<int>(c);
			}
		} else if (!stats && column.kind == Kind::Mod) {
			uint16_t id = 0, kind = 0;
			memcpy(&id, row + at, sizeof(id));
			memcpy(&kind, row + at + 2, sizeof(kind));
			if (id != 0xffff && kind == 0 && id >= from) { // kind != 0: a property group
				return static_cast<int>(c);
			}
		}
	}
	return -1;
}

namespace {

// A line's record, compiled once for its cells (RecordKey); nullptr when the game refuses it.
const std::vector<uint8_t>* Compiled(Game& game, const Layout& layout, const Sheet& sheet, size_t line, Cache& cache) {
	if (!sheet.usable) {
		return nullptr;
	}
	CellViews   cells;
	std::string key;
	cells.reserve(kColumnCount + 8);
	SplitCells(sheet.lines[line], cells);
	RecordKey(sheet.header, cells, key);
	auto held = cache.find(key);
	if (held == cache.end()) {
		std::vector<uint8_t> row(layout.rowSize);
		std::string          problem;
		const bool           ok = CompileLine(game, layout, sheet.header, sheet.lines[line], static_cast<int>(line), row.data(), problem);
		held                    = cache.emplace(std::move(key), ok ? std::move(row) : std::vector<uint8_t> {}).first;
	}
	return held->second.empty() ? nullptr : &held->second;
}

}

Outcome ExactList(Game& game, const Layout& layout, const Sheet& mine, Cache& cache, const uint8_t* extra, size_t extraCount) {
	Outcome out;
	out.status.assign(mine.lines.size(), LineStatus::Blank);
	std::vector<const uint8_t*> list;
	list.reserve(mine.lines.size() + extraCount);
	std::vector<uint8_t> row(layout.rowSize);
	std::string          problem;
	for (size_t i = 0; i < mine.lines.size(); ++i) {
		const std::string_view text = mine.lines[i];
		if (IsBlank(text)) {
			continue;
		}
		if (SameName(Cell(text, mine.header.action), "remove")) {
			out.status[i] = LineStatus::Idle; // a remove line of an older file: nothing to take out of a whole list
			++out.idle;
			continue;
		}
		const std::vector<uint8_t>* record = Compiled(game, layout, mine, i, cache);
		if (record == nullptr) {
			++out.refused;
			out.status[i] = LineStatus::Refused;
			if (out.problems.size() < 20) {
				if (!mine.usable) {
					problem = "the file's first line lacks the columns a recipe needs";
				} else {
					CompileLine(game, layout, mine.header, text, static_cast<int>(i), row.data(), problem);
				}
				out.problems.push_back(Describe(mine, i) + ": " + problem);
			}
			continue;
		}
		list.push_back(record->data());
		for (std::string& note : AmountNotes(mine.header, text)) {
			if (out.notes.size() < 20) {
				out.notes.push_back(Describe(mine, i) + ": " + note);
			}
		}
		out.status[i] = LineStatus::Added;
		++out.added;
		out.disabled += (*record)[0] == 0 ? 1 : 0;
	}
	out.count = list.size() + extraCount;
	out.rows.resize(out.count * layout.rowSize);
	for (size_t i = 0; i < list.size(); ++i) {
		memcpy(out.rows.data() + i * layout.rowSize, list[i], layout.rowSize);
	}
	if (extraCount != 0) {
		memcpy(out.rows.data() + list.size() * layout.rowSize, extra, extraCount * layout.rowSize);
	}
	return out;
}

namespace {

uint64_t WholeHash(const Layout& layout, const uint8_t* row) {
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < layout.rowSize; ++i) {
		hash = (hash ^ (IsLineNumberByte(layout, i) ? 0 : row[i])) * 1099511628211ull;
	}
	return hash;
}

}

DefaultsIndex IndexDefaults(Game& game, const Layout& layout, const Sheet& defaults, Cache& cache) {
	DefaultsIndex index;
	for (size_t i = 0; i < defaults.lines.size(); ++i) {
		if (const std::vector<uint8_t>* record = IsBlank(defaults.lines[i]) ? nullptr : Compiled(game, layout, defaults, i, cache)) {
			index.ingredients[IngredientsHash(layout, record->data())].push_back(record->data());
			index.whole[WholeHash(layout, record->data())].push_back(record->data());
		}
	}
	return index;
}

LineFacts FactsOf(Game& game, const Layout& layout, const DefaultsIndex& index, const Sheet& mine, size_t line, Cache& cache) {
	LineFacts facts;
	if (line >= mine.lines.size() || IsBlank(mine.lines[line])) {
		return facts;
	}
	const std::vector<uint8_t>* record = Compiled(game, layout, mine, line, cache);
	if (record == nullptr) {
		facts.status = LineStatus::Refused;
		facts.own    = true; // a line the game cannot read is the user's doing
		return facts;
	}
	facts.status = SameName(Cell(mine.lines[line], mine.header.action), "remove") ? LineStatus::Idle : LineStatus::Added;
	facts.own    = true;
	if (const auto bucket = index.whole.find(WholeHash(layout, record->data())); bucket != index.whole.end()) {
		for (const uint8_t* other : bucket->second) {
			facts.own = facts.own && !SameRecipe(layout, other, record->data());
		}
	}
	if (const auto bucket = index.ingredients.find(IngredientsHash(layout, record->data())); bucket != index.ingredients.end()) {
		for (const uint8_t* other : bucket->second) {
			facts.isDefault = facts.isDefault || SameIngredients(layout, other, record->data());
		}
	}
	return facts;
}

Compared Compare(Game& game, const Layout& layout, const Sheet& defaults, const Sheet& mine, const Sheet& deleted, Cache& cache) {
	Compared out;
	const auto whole = [&](const uint8_t* row) {
		uint64_t hash = 14695981039346656037ull;
		for (size_t i = 0; i < layout.rowSize; ++i) {
			hash = (hash ^ (IsLineNumberByte(layout, i) ? 0 : row[i])) * 1099511628211ull;
		}
		return hash;
	};
	// the ingredients in the user's list and in the notes of deleted recipes
	std::unordered_map<uint64_t, std::vector<const uint8_t*>> inMine, inDeleted, defaultIngredients, defaultWhole;
	const auto has = [&](const std::unordered_map<uint64_t, std::vector<const uint8_t*>>& set, const uint8_t* record, bool exactly) {
		const auto bucket = set.find(exactly ? whole(record) : IngredientsHash(layout, record));
		if (bucket != set.end()) {
			for (const uint8_t* other : bucket->second) {
				if (exactly ? SameRecipe(layout, other, record) : SameIngredients(layout, other, record)) {
					return true;
				}
			}
		}
		return false;
	};
	std::vector<const std::vector<uint8_t>*> mineRecords(mine.lines.size(), nullptr);
	for (size_t i = 0; i < mine.lines.size(); ++i) {
		if (!IsBlank(mine.lines[i]) && (mineRecords[i] = Compiled(game, layout, mine, i, cache)) != nullptr) {
			inMine[IngredientsHash(layout, mineRecords[i]->data())].push_back(mineRecords[i]->data());
		}
	}
	for (size_t i = 0; i < deleted.lines.size(); ++i) {
		if (const std::vector<uint8_t>* record = IsBlank(deleted.lines[i]) ? nullptr : Compiled(game, layout, deleted, i, cache)) {
			inDeleted[IngredientsHash(layout, record->data())].push_back(record->data());
		}
	}
	for (size_t i = 0; i < defaults.lines.size(); ++i) {
		const std::vector<uint8_t>* record = IsBlank(defaults.lines[i]) ? nullptr : Compiled(game, layout, defaults, i, cache);
		if (record == nullptr) {
			continue;
		}
		defaultIngredients[IngredientsHash(layout, record->data())].push_back(record->data());
		defaultWhole[whole(record->data())].push_back(record->data());
		if (!has(inMine, record->data(), false)) {
			(has(inDeleted, record->data(), false) ? out.deleted : out.fresh).push_back(i);
		}
	}
	out.own.assign(mine.lines.size(), 0);
	out.isDefault.assign(mine.lines.size(), 0);
	for (size_t i = 0; i < mine.lines.size(); ++i) {
		if (mineRecords[i] != nullptr) {
			out.own[i]       = has(defaultWhole, mineRecords[i]->data(), true) ? 0 : 1;
			out.isDefault[i] = has(defaultIngredients, mineRecords[i]->data(), false) ? 1 : 0;
		} else if (!IsBlank(mine.lines[i])) {
			out.own[i] = 1; // a line the game cannot read is the user's doing
		}
	}
	return out;
}

std::string ResultFirst(std::string_view description) {
	const size_t arrow = description.rfind(" -> ");
	if (arrow == std::string_view::npos || description.find(" <- ") != std::string_view::npos) {
		return std::string(description);
	}
	const std::string_view made = Trim(description.substr(arrow + 4)), from = Trim(description.substr(0, arrow));
	if (made.empty() || from.empty()) {
		return std::string(description);
	}
	return std::string(made) + " <- " + std::string(from);
}

Names ReadNames(const LegendTables& tables) {
	Names      names;
	const auto cell = [](const std::vector<std::string_view>& row, int at) { return at >= 0 && static_cast<size_t>(at) < row.size() ? std::string(row[static_cast<size_t>(at)]) : std::string(); };
	for (const std::string& text : tables.items) {
		const LegendTable table = ReadLegendTable(text);
		const int         code = table.column("code"), name = table.column("name");
		for (const auto& row : table.rows) {
			if (!cell(row, code).empty() && !cell(row, name).empty()) {
				names.items.emplace(cell(row, code), cell(row, name));
			}
		}
	}
	for (const std::string& text : tables.types) {
		const LegendTable table = ReadLegendTable(text);
		const int         code = table.column("Code"), name = table.column("ItemType");
		for (const auto& row : table.rows) {
			if (!cell(row, code).empty() && !cell(row, name).empty()) {
				names.types.emplace(cell(row, code), cell(row, name));
			}
		}
	}
	return names;
}

namespace {

// One input or output cell in words: "3 Thul Rune", "Rare Any Shield (4 sockets)".
std::string CellInWords(const std::string& cell, const Names& names, const std::string& inputOne) {
	std::vector<std::string> words;
	std::string              word;
	for (size_t i = 0; i <= cell.size(); ++i) {
		const char c = i < cell.size() ? cell[i] : ',';
		if (c == ',') {
			words.push_back(std::string(Trim(word)));
			word.clear();
		} else if (c != '"') {
			word += c;
		}
	}
	if (words.empty() || words[0].empty()) {
		return {};
	}
	std::string what = words[0];
	if (const auto item = names.items.find(what); item != names.items.end()) {
		what = item->second;
	} else if (const auto type = names.types.find(what); type != names.types.end()) {
		what = type->second;
	} else if (what == "useitem") {
		what = inputOne.empty() ? "the item" : inputOne;
	} else if (what == "usetype") {
		what = "new " + (inputOne.empty() ? std::string("item of the same type") : inputOne);
	} else if (what == "any") {
		what = "any item";
	}
	std::string before, after, count;
	for (size_t i = 1; i < words.size(); ++i) {
		const std::string& w = words[i];
		const char*        quality = w == "low" ? "Low Quality" : w == "nor" ? "Normal" : w == "hiq" ? "Superior" : w == "mag" ? "Magic" : w == "rar" ? "Rare" : w == "set" ? "Set" : w == "uni" ? "Unique"
		                           : w == "crf" ? "Crafted" : w == "tmp" ? "Tempered" : w == "eth" ? "Ethereal" : w == "bas" ? "Basic" : w == "exc" ? "Exceptional" : w == "eli" ? "Elite" : nullptr;
		if (quality != nullptr) {
			before += std::string(quality) + " ";
		} else if (w.rfind("qty=", 0) == 0) {
			count = w.substr(4) + " ";
		} else if (w.rfind("sock=", 0) == 0) {
			after += " (" + w.substr(5) + " sockets)";
		} else if (w == "nos") {
			after += " (no sockets)";
		}
	}
	return count + before + what + after;
}

}

std::string GenericName(const Cells& cells, const Names& names) {
	std::string inputs, outputs, inputOne;
	for (size_t c = 0; c < kColumnCount && c < cells.cell.size(); ++c) { // the inputs come first in the columns
		if (cells.cell[c].empty()) {
			continue;
		}
		if (kColumns[c].kind == Kind::Input) {
			const std::string words = CellInWords(cells.cell[c], names, {});
			if (inputOne.empty()) {
				inputOne = words;
			}
			inputs += (inputs.empty() ? "" : " + ") + words;
		} else if (kColumns[c].kind == Kind::Output) {
			outputs += (outputs.empty() ? "" : " + ") + CellInWords(cells.cell[c], names, inputOne);
		}
	}
	return (outputs.empty() ? std::string("?") : outputs) + " <- " + (inputs.empty() ? std::string("?") : inputs);
}

std::string ButtonSprite(std::string_view source, int width, int height) {
	constexpr size_t kHeader = 40;
	const auto       u16     = [&](size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(source[at])) | static_cast<uint32_t>(static_cast<uint8_t>(source[at + 1])) << 8; };
	const auto       u32     = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
	if (source.size() <= kHeader || source.substr(0, 4) != "SpA1" || width < 8 || height < 4 || width > 4096 || height > 1024) {
		return {};
	}
	const uint32_t frameWidth = u16(6), allWidth = u32(8), srcHeight = u32(12), frames = u32(20);
	if (frames == 0 || frames > 16 || frameWidth < 8 || srcHeight < 4 || srcHeight > 4096 || allWidth < frames * frameWidth || allWidth > 65536
		|| source.size() < kHeader + static_cast<size_t>(allWidth) * srcHeight * 4) {
		return {};
	}
	const uint32_t gap    = frames > 1 ? (allWidth - frames * frameWidth) / (frames - 1) : 0;
	const double   scale  = static_cast<double>(srcHeight) / height;           // source pixels a pixel of the result
	const int      scaled = static_cast<int>(frameWidth / scale + 0.5);        // a frame's width at the new height
	int            cap    = static_cast<int>(srcHeight * 0.62 / scale + 0.5);  // an end: the corner ornaments
	cap                   = std::min(cap, std::min(width / 2, (scaled - 2) / 2));
	if (scaled < 4 || cap < 1) {
		return {};
	}
	const int   middle = scaled - 2 * cap;
	const auto* pixels = reinterpret_cast<const uint8_t*>(source.data()) + kHeader;
	const int   outAll = width * static_cast<int>(frames);
	std::string out(kHeader + static_cast<size_t>(outAll) * height * 4, '\0');
	const auto  put16 = [&](size_t at, uint32_t v) { out[at] = static_cast<char>(v); out[at + 1] = static_cast<char>(v >> 8); };
	const auto  put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
	out.replace(0, 4, "SpA1");
	put16(4, 31);
	put16(6, static_cast<uint32_t>(width));
	put32(8, static_cast<uint32_t>(outAll));
	put32(12, static_cast<uint32_t>(height));
	put32(20, frames);
	put32(32, static_cast<uint32_t>(out.size() - kHeader));
	put32(36, 4);
	for (uint32_t f = 0; f < frames; ++f) {
		const uint32_t frameAt = f * (frameWidth + gap);
		for (int x = 0; x < width; ++x) {
			int sx; // the column of the scaled frame this column shows
			if (x < cap) {
				sx = x;
			} else if (x >= width - cap) {
				sx = scaled - (width - x);
			} else {
				const int t = (x - cap) % (2 * middle);
				sx          = cap + (t < middle ? t : 2 * middle - 1 - t);
			}
			const uint32_t x0 = std::min(frameWidth - 1, static_cast<uint32_t>(sx * scale));
			const uint32_t x1 = std::min(frameWidth, std::max(x0 + 1, static_cast<uint32_t>((sx + 1) * scale + 0.5)));
			for (int y = 0; y < height; ++y) {
				const uint32_t y0 = std::min(srcHeight - 1, static_cast<uint32_t>(y * scale));
				const uint32_t y1 = std::min(srcHeight, std::max(y0 + 1, static_cast<uint32_t>((y + 1) * scale + 0.5)));
				uint64_t       sum[4] {}; // colours weighed by how solid they are
				for (uint32_t v = y0; v < y1; ++v) {
					const uint8_t* p = pixels + (static_cast<size_t>(v) * allWidth + frameAt + x0) * 4;
					for (uint32_t u = x0; u < x1; ++u, p += 4) {
						sum[0] += static_cast<uint64_t>(p[0]) * p[3];
						sum[1] += static_cast<uint64_t>(p[1]) * p[3];
						sum[2] += static_cast<uint64_t>(p[2]) * p[3];
						sum[3] += p[3];
					}
				}
				const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
				char*          to    = out.data() + kHeader + (static_cast<size_t>(y) * outAll + f * static_cast<uint32_t>(width) + static_cast<uint32_t>(x)) * 4;
				if (sum[3] != 0) {
					to[0] = static_cast<char>(sum[0] / sum[3]);
					to[1] = static_cast<char>(sum[1] / sum[3]);
					to[2] = static_cast<char>(sum[2] / sum[3]);
					to[3] = static_cast<char>(sum[3] / count);
				}
			}
		}
	}
	return out;
}

}
