// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The save reader against COPIES of real saves (never the live files) and the mod's tables as text.
// usage: core_test <folder with copies of .d2s/.d2i> <folder with itemstatcost/armor/weapons/misc.txt>
#include "../fix_core.h"
#include "../save_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

using namespace savescan;

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

static std::string Slurp(const std::filesystem::path& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

static FileResult Read(const std::filesystem::path& path, const std::string& bytes, const Tables& tables) {
	const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
	return path.extension() == ".d2i" ? ReadStash(data, bytes.size(), tables) : ReadCharacter(data, bytes.size(), tables);
}


// Live Properties' stat-edits.txt: "number<TAB>signed<TAB>send bits<TAB>save bits<TAB>save add<TAB>..." (- = the row's own)
static std::vector<ResizeSpec> StatEdits(const std::string& text, const Tables& tables) {
	std::vector<ResizeSpec> out;
	std::istringstream      in(text);
	std::string             line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> cells;
		std::istringstream       row(line);
		std::string              cell;
		while (std::getline(row, cell, '\t')) {
			cells.push_back(cell);
		}
		if (cells.size() < 5) {
			continue;
		}
		const int number = atoi(cells[0].c_str());
		if (number < 0 || static_cast<size_t>(number) >= tables.stats.size()) {
			continue;
		}
		const StatShape& now = tables.stats[static_cast<size_t>(number)];
		ResizeSpec       spec { static_cast<uint16_t>(number), now.saveBits, now.paramBits, now.saveAdd };
		if (cells[3] != "-") {
			spec.saveBits = static_cast<uint8_t>(atoi(cells[3].c_str()));
		}
		if (cells[4] != "-") {
			spec.saveAdd = atoi(cells[4].c_str());
		}
		out.push_back(spec);
	}
	return out;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: core_test <saves copy> <excel folder>\n");
		return 2;
	}
	const std::filesystem::path saves = argv[1], excel = argv[2];
	Tables                      tables;
	ShapesFromText(tables, Slurp(excel / "itemstatcost.txt"));
	ReadItemTable(tables, Slurp(excel / "armor.txt"), kArmor);
	ReadItemTable(tables, Slurp(excel / "weapons.txt"), kWeapon);
	ReadItemTable(tables, Slurp(excel / "misc.txt"), kMisc);
	const size_t textRows = tables.stats.size();
	if (argc > 3 && std::filesystem::exists(argv[3])) { // the rows Live Tables made: the game has them too
		for (const MadeRow& row : MadeStatRows(Slurp(argv[3]))) {
			if (tables.stats.size() <= row.number) {
				tables.stats.resize(static_cast<size_t>(row.number) + 1);
			}
			tables.stats[row.number] = row.shape;
		}
		printf("      %zu made rows from %s (the table: %zu text rows -> %zu)\n", tables.stats.size() - textRows, argv[3], textRows, tables.stats.size());
	}
	if (argc > 4 && std::filesystem::exists(argv[4])) { // and the shapes Live Properties changed in memory
		const std::vector<ResizeSpec> edits = StatEdits(Slurp(argv[4]), tables);
		std::string                   bad;
		ApplyBefore(tables, edits, bad);
		printf("      %zu stat edits of Live Properties applied (copy of stat-edits.txt)\n", edits.size());
	}
	Check(tables.stats.size() > 359 && tables.stats[7].saveBits != 0 && tables.items.count("hp1") == 1 && tables.items["cap"].kind == kArmor && tables.items["cm1"].charm && tables.items["tbk"].tome,
		"tables: " + std::to_string(tables.stats.size()) + " stats, " + std::to_string(tables.items.size()) + " item codes");

	const auto is = [&](size_t stat, int bits, int param, int add) { return tables.stats[stat].saveBits == bits && tables.stats[stat].paramBits == param && tables.stats[stat].saveAdd == add; };
	Check(is(97, 6, 9, 0) && is(188, 3, 16, 0) && is(204, 16, 16, 0) && is(31, 11, 0, 10), "the four stats the plugin checks the game's row layout with have their usual shape in this mod");

	// the rows the game holds = the same shapes
	std::vector<uint8_t> rows(tables.stats.size() * kStatRow, 0);
	for (size_t i = 0; i < tables.stats.size(); ++i) {
		rows[i * kStatRow + kRowSaveBits] = tables.stats[i].saveBits;
		const int32_t add = tables.stats[i].saveAdd, param = tables.stats[i].paramBits;
		memcpy(rows.data() + i * kStatRow + kRowSaveAdd, &add, 4);
		memcpy(rows.data() + i * kStatRow + kRowSaveParam, &param, 4);
	}
	Tables fromRows = tables;
	ShapesFromRows(fromRows, rows.data(), tables.stats.size());
	bool same = fromRows.stats.size() == tables.stats.size();
	for (size_t i = 0; same && i < tables.stats.size(); ++i) {
		same = fromRows.stats[i].saveBits == tables.stats[i].saveBits && fromRows.stats[i].saveAdd == tables.stats[i].saveAdd && fromRows.stats[i].paramBits == tables.stats[i].paramBits;
	}
	Check(same, "stat shapes taken from table rows = the ones from the text");

	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(saves)) {
		if (entry.path().extension() == ".d2s" || entry.path().extension() == ".d2i") {
			files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end());
	Check(!files.empty(), std::to_string(files.size()) + " save files to read");
	size_t                 all = 0, tomes = 0, charms = 0, recorded = 0, sets = 0, runewords = 0, socketed = 0, stacked = 0, named = 0, ears = 0, withStats = 0;
	std::map<uint16_t, int> holders; // stat -> items that have it
	std::mt19937           random(12345);
	for (const auto& path : files) {
		const std::string bytes  = Slurp(path);
		const FileResult  result = Read(path, bytes, tables);
		std::map<std::string, int> sections;
		for (const Item& item : result.items) {
			++sections[item.section];
			const auto kind = tables.items.find(item.code);
			tomes     += kind != tables.items.end() && kind->second.tome ? 1 : 0;
			charms    += kind != tables.items.end() && kind->second.charm && item.quality == 2 ? 1 : 0;
			recorded  += item.flags >> 28 & 1;
			runewords += item.flags >> 26 & 1;
			named     += item.flags >> 24 & 1;
			ears      += item.flags >> 16 & 1;
			sets      += item.quality == 5 ? 1 : 0;
			socketed  += item.inSocket ? 1 : 0;
			stacked   += item.quantity != 0 ? 1 : 0;
			withStats += item.stats.empty() ? 0 : 1;
			std::map<uint16_t, bool> seen;
			for (const StatValue& stat : item.stats) {
				if (!seen[stat.stat]) {
					++holders[stat.stat];
					seen[stat.stat] = true;
				}
			}
		}
		std::string where;
		for (const auto& [section, count] : sections) {
			where += (where.empty() ? "" : ", ") + section + " " + std::to_string(count);
		}
		all += result.items.size();
		Check(result.ok, path.filename().string() + ": " + std::to_string(result.items.size()) + " items read to the end marker" + (where.empty() ? "" : " (" + where + ")") + (result.ok ? "" : " - " + result.problem));

		// cut short: must say so, never "fine"
		if (bytes.size() > 1200 && result.ok && !result.items.empty()) {
			const std::string cut  = bytes.substr(0, bytes.size() - bytes.size() / 3);
			const FileResult  less = Read(path, cut, tables);
			if (!less.ok && !less.problem.empty()) {
			} else {
				Check(false, path.filename().string() + ": a file cut short was not reported as unreadable");
			}
		}
		// damaged bytes: must never crash (the answer may be either)
		for (int n = 0; n < 300; ++n) {
			std::string damaged = bytes;
			for (int k = 0; k < 1 + n % 4; ++k) {
				damaged[random() % damaged.size()] = static_cast<char>(random());
			}
			static_cast<void>(Read(path, damaged, tables));
		}
	}
	printf("      %zu items in all: %zu with stats, %zu in sockets, %zu with a quantity, %zu tomes, %zu normal-quality charms, %zu set items, %zu runewords, %zu flag-28 items, %zu personalized, %zu ears\n", all,
		withStats, socketed, stacked, tomes, charms, sets, runewords, recorded, named, ears);
	int live = 0;
	for (uint16_t stat = 479; stat <= 486; ++stat) {
		live += holders.count(stat) != 0 ? holders[stat] : 0;
	}
	printf("      items holding a Live Properties stat (479-486): %d\n", live);

	// a stat whose saved shape is not what the table says: the file is unreadable, not "clean"
	if (!holders.empty()) {
		uint16_t common = holders.begin()->first;
		for (const auto& [stat, count] : holders) {
			common = count > holders[common] ? stat : common;
		}
		Tables wrong = tables;
		wrong.stats[common].saveBits = static_cast<uint8_t>(wrong.stats[common].saveBits + 3);
		int unreadable = 0, holding = 0;
		for (const auto& path : files) {
			const std::string bytes = Slurp(path);
			const FileResult  good  = Read(path, bytes, tables);
			bool              has   = false;
			for (const Item& item : good.items) {
				for (const StatValue& stat : item.stats) {
					has = has || stat.stat == common;
				}
			}
			holding    += has ? 1 : 0;
			unreadable += has && !Read(path, bytes, wrong).ok ? 1 : 0;
		}
		Check(holding > 0 && unreadable == holding, "stat " + std::to_string(common) + " given 3 more save bits: all " + std::to_string(holding) + " files that hold it turn unreadable (" + std::to_string(unreadable) + ")");
	}
	Tables none = tables;
	none.items.erase("hp1");
	int refused = 0, had = 0;
	for (const auto& path : files) {
		const std::string bytes = Slurp(path);
		bool              has   = false;
		for (const Item& item : Read(path, bytes, tables).items) {
			has = has || item.code == "hp1";
		}
		had     += has ? 1 : 0;
		refused += has && !Read(path, bytes, none).ok ? 1 : 0;
	}
	Check(had == refused, "an item code missing from the tables makes its file unreadable (" + std::to_string(refused) + " of " + std::to_string(had) + ")");

	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
