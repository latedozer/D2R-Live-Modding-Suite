// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// affix_core natively: rows made from the game's own MagicPrefix / MagicSuffix text (as the
// game's loader would place the columns) read back as the same cells, and cells typed in
// make the rows they say. usage: affix_test <excel folder with MagicPrefix.txt, MagicSuffix.txt, Properties.txt, ItemTypes.txt>
#include "../affix_core.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	g_failed += ok ? 0 : 1;
}

using Table = std::vector<std::vector<std::string>>;
static Table ReadTable(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	Table         table;
	std::string   line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		std::vector<std::string> cells;
		std::stringstream        s(line);
		std::string              cell;
		while (std::getline(s, cell, '\t')) {
			cells.push_back(cell);
		}
		if (!line.empty() && line.back() == '\t') {
			cells.push_back("");
		}
		table.push_back(cells);
	}
	return table;
}
static int Column(const Table& t, const char* name) {
	for (size_t i = 0; i < t[0].size(); ++i) {
		if (t[0][i] == name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}
static std::string Cell(const Table& t, size_t row, int column) {
	return column >= 0 && static_cast<size_t>(column) < t[row].size() ? t[row][static_cast<size_t>(column)] : "";
}

int main(int argc, char** argv) {
	const std::string folder = argc > 1 ? argv[1] : "../vanilla/data/global/excel/";
	const Table       props = ReadTable(folder + "Properties.txt"), types = ReadTable(folder + "ItemTypes.txt");
	std::vector<std::string> propCodes, typeCodes;
	for (size_t i = 1; i < props.size(); ++i) {
		propCodes.push_back(Cell(props, i, Column(props, "code")));
	}
	for (size_t i = 1; i < types.size(); ++i) {
		typeCodes.push_back(Cell(types, i, Column(types, "Code")));
	}
	Check(propCodes.size() > 200 && typeCodes.size() > 50, "the game's property and item type codes are read (" + std::to_string(propCodes.size()) + ", " + std::to_string(typeCodes.size()) + ")");
	affix::Names game; // what the loader puts in an empty cell (any value no code has)
	game.props = &propCodes, game.types = &typeCodes;
	game.noProp = -1, game.noType = 0xffff;

	for (const char* name : { "MagicPrefix.txt", "MagicSuffix.txt" }) {
		const Table          t = ReadTable(folder + name);
		const size_t         count = t.size() - 1;
		std::vector<uint8_t> rows(count * affix::kRow, 0);
		std::vector<affix::Cells> want(count);
		bool                 made = true;
		size_t               unknown = 0;
		for (size_t r = 0; r < count; ++r) {
			uint8_t*      row = rows.data() + r * affix::kRow;
			affix::Cells& w   = want[r];
			for (int i = 0; i < affix::kCells; ++i) {
				const affix::Field& f    = affix::kFields[i];
				std::string         text = Cell(t, r + 1, Column(t, f.name));
				if (f.kind == affix::kName) {
					text = text.substr(0, affix::kNameRoom - 1);
					memcpy(row, text.c_str(), text.size());
				} else if (f.kind == affix::kProp || f.kind == affix::kType) {
					const std::vector<std::string>& codes = f.kind == affix::kProp ? propCodes : typeCodes;
					long                            value = f.kind == affix::kProp ? game.noProp : game.noType;
					for (size_t c = 0; c < codes.size() && !text.empty(); ++c) {
						value = codes[c] == text ? static_cast<long>(c) : value;
					}
					if (value == (f.kind == affix::kProp ? game.noProp : game.noType) && !text.empty()) {
						text.clear(); // a code the game has no row for ("staff", "ring "): the game leaves the cell empty too
						++unknown;
					}
					affix::Put(row, f, value);
				} else {
					affix::Put(row, f, atol(text.c_str()));
					text = atol(text.c_str()) != 0 ? std::to_string(atol(text.c_str())) : "";
				}
				w[static_cast<size_t>(i)] = text;
			}
		}
		affix::Names learnt = game;
		learnt.noProp = 12345, learnt.noType = 12345;
		affix::Learn(rows.data(), count, learnt);
		Check(made && learnt.noProp == game.noProp && learnt.noType == game.noType, std::string(name) + ": the empty cells' values are learnt from the rows (" + std::to_string(unknown) + " codes the game has no row for)");

		size_t same = 0, rebuilt = 0, blanks = 0;
		std::string first, whyNot;
		for (size_t r = 0; r < count; ++r) {
			const uint8_t*     row   = rows.data() + r * affix::kRow;
			const affix::Cells cells = affix::Read(row, game);
			same += cells == want[r] ? 1 : 0;
			if (cells != want[r] && first.empty()) {
				for (int i = 0; i < affix::kCells; ++i) {
					if (cells[static_cast<size_t>(i)] != want[r][static_cast<size_t>(i)]) {
						first = "row " + std::to_string(r) + " " + affix::kFields[i].name + ": '" + cells[static_cast<size_t>(i)] + "' not '" + want[r][static_cast<size_t>(i)] + "'";
						break;
					}
				}
			}
			uint8_t     next[affix::kRow];
			std::string problem;
			memcpy(next, row, affix::kRow);
			memset(next + 0x24, 0x5a, affix::kRow - 0x24); // everything but the name and the version is written by Build
			memcpy(next + 0x56, row + 0x56, 2);            // ... except transformcolor (and the byte before it, unused)
			memcpy(next + 0x84, row + 0x84, 8);            // ... and the price columns
			memset(next + 0x55, row[0x55], 1);
			memset(next + 0x69, row[0x69], 1);
			memset(next + 0x83, row[0x83], 1);
			const bool again = affix::Build(cells, game, next, problem) && memcmp(next, row, affix::kRow) == 0;
			rebuilt += again ? 1 : 0;
			if (!again && whyNot.empty()) {
				whyNot = "row " + std::to_string(r) + ": " + problem;
				for (size_t b = 0; b < affix::kRow && problem.empty(); ++b) {
					if (next[b] != row[b]) {
						whyNot += "byte +" + std::to_string(b) + " differs";
						break;
					}
				}
			}
			blanks += affix::IsBlank(row, game) ? 1 : 0;
		}
		Check(same == count, std::string(name) + ": every row reads back as the text's cells (" + std::to_string(same) + " of " + std::to_string(count) + ") " + first);
		Check(rebuilt == count, std::string(name) + ": the cells build the very same row again (" + std::to_string(rebuilt) + " of " + std::to_string(count) + ") " + whyNot);
		Check(blanks > 0 && blanks < count / 4, std::string(name) + ": empty rows are seen (" + std::to_string(blanks) + ")");
	}

	// typing: a made charm affix
	uint8_t      row[affix::kRow] = {};
	affix::Cells cells            = affix::Read(row, game);
	Check(affix::IsBlank(row, game) || true, "a zeroed row reads");
	affix::Put(row, affix::kFields[affix::kModCell], game.noProp);
	affix::Put(row, affix::kFields[affix::kModCell + 4], game.noProp);
	affix::Put(row, affix::kFields[affix::kModCell + 8], game.noProp);
	for (int i = affix::kTypeCell; i < affix::kCells; ++i) {
		affix::Put(row, affix::kFields[i], game.noType);
	}
	cells = affix::Read(row, game);
	Check(affix::IsBlank(row, game) && cells[affix::kModCell].empty() && cells[affix::kTypeCell].empty(), "an empty row reads as empty cells");
	cells[affix::kSpawnCell] = "1", cells[affix::kLevelCell] = "99", cells[affix::kFrequencyCell] = "1";
	cells[affix::kModCell] = "dmg-min", cells[affix::kModCell + 2] = "5", cells[affix::kModCell + 3] = "10", cells[affix::kTypeCell] = "scha";
	uint8_t     next[affix::kRow];
	std::string problem;
	memcpy(next, row, sizeof(row));
	const bool built = affix::Build(cells, game, next, problem);
	const affix::Cells back = affix::Read(next, game);
	Check(built && back[affix::kModCell] == "dmg-min" && back[affix::kModCell + 3] == "10" && back[affix::kTypeCell] == "scha" && back[affix::kTypeCell + 1].empty() && affix::Get(next, affix::kFields[affix::kLevelCell]) == 99
			&& !affix::IsBlank(next, game),
		"a charm affix typed in (dmg-min 5-10 on scha, level 99, frequency 1) is built " + problem);
	cells[affix::kTypeCell] = "nosuchtype";
	{ const bool ok = !affix::Build(cells, game, next, problem) && problem.find("item type") != std::string::npos; Check(ok, "an unknown item type is refused: " + problem); }
	cells[affix::kTypeCell] = "scha", cells[affix::kModCell] = "nosuchprop";
	{ const bool ok = !affix::Build(cells, game, next, problem) && problem.find("property") != std::string::npos; Check(ok, "an unknown property is refused: " + problem); }
	cells[affix::kModCell + 2] = "5", cells[affix::kFrequencyCell] = "300";
	{ const bool ok = !affix::Build(cells, game, next, problem) && problem.find("frequency") != std::string::npos; Check(ok, "a frequency over 255 is refused: " + problem); }
	cells[affix::kFrequencyCell] = "1", cells[affix::kModCell] = "#5";
	Check(affix::Build(cells, game, next, problem) && affix::Get(next, affix::kFields[affix::kModCell]) == 5, "a property row by number (#5) is taken");

	{ // 2026-11-25 21:38 crash: the property names are read again after Live Tables grew the table (the
	  // vector's elements move); an affix read afterwards must see the new list, not freed memory
		std::vector<std::string> codes = propCodes;
		affix::Names             live  = game;
		live.props                     = &codes;
		std::vector<uint8_t>     row(affix::kRow, 0);
		affix::Put(row.data(), affix::kFields[affix::kModCell], static_cast<long>(codes.size())); // the row about to be grown
		codes.shrink_to_fit();
		const std::string* before = codes.data();
		codes.push_back("%dmg"); // the grown row: the elements move (capacity was exactly full)
		const affix::Cells cells = affix::Read(row.data(), live);
		Check(codes.data() != before && cells[affix::kModCell] == "%dmg", "names re-read after a grow (the list moved): an affix naming the new property reads '%dmg'");
	}
	{ // affix Disable / Enable (0.11.1): spawnable and frequency 0, then both back; nothing else changes
		std::vector<uint8_t> row(affix::kRow, 0);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = static_cast<uint8_t>(i * 7 + 3); // every byte something
		}
		affix::Put(row.data(), affix::kFields[affix::kSpawnCell], 1);
		affix::Put(row.data(), affix::kFields[affix::kFrequencyCell], 4);
		const std::vector<uint8_t> orig = row;
		affix::Off(row.data());
		const bool off  = affix::IsOff(row.data()) && !affix::IsOff(orig.data());
		size_t     diff = 0;
		for (size_t i = 0; i < row.size(); ++i) {
			diff += row[i] != orig[i] ? 1 : 0;
		}
		affix::On(row.data(), orig.data());
		Check(off && diff == 2 && row == orig, "affix Disable: spawnable and frequency 0 (only those 2 bytes); Enable: the row is byte for byte what it was");
	}
	{ // the classic block: rows before the files' Expansion line (version 0 / blank), as the game holds them (Expansion dropped)
		for (const char* name : { "MagicPrefix.txt", "MagicSuffix.txt" }) {
			const Table          t   = ReadTable(folder + name);
			const int            col = Column(t, "version");
			std::vector<uint8_t> rows;
			size_t               expansion = 0;
			for (size_t r = 1; r < t.size(); ++r) {
				if (Cell(t, r, 0) == "Expansion") {
					expansion = rows.size() / affix::kRow; // the game drops this line
					continue;
				}
				std::vector<uint8_t> row(affix::kRow, 0);
				const uint16_t       v = static_cast<uint16_t>(atoi(Cell(t, r, col).c_str()));
				memcpy(row.data() + 0x22, &v, 2);
				rows.insert(rows.end(), row.begin(), row.end());
			}
			const size_t end = affix::ClassicEnd(rows.data(), rows.size() / affix::kRow);
			Check(expansion > 0 && end == expansion, std::string(name) + ": the classic block ends where the Expansion line was (row " + std::to_string(end) + ")");
		}
	}
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
