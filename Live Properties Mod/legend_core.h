// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The Legend's sections of Live Properties (Mod Book 0.8.0): Properties, Stats, Affixes. The words
// are the game's own data guide's (data/global/DataGuide in the game's files), shortened, plus what
// this plugin found out; the plugin adds examples from the tables the game read (Examples).
// No game memory here: tested natively (test/legend_test.cpp).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace legend {

constexpr size_t kRowMost = 64; // a list row is one line, "code - meaning", which Mod Book cuts at about 64 letters

struct Item {
	std::string code, meaning, detail, insert; // insert: what the Legend's Insert puts into the cell being edited ("" = none)
};
struct Section {
	std::vector<Item> how;  // "How it works", shown above the list
	std::vector<Item> list;
};

// "In your tables: ..." lines, by an entry's code ("func 1", "val shift", "op 2", "level" ...).
using Examples = std::map<std::string, std::string>;

// funcs: how many funcs the game has; works(f): the game has a handler for func f.
Section Properties(int funcs, bool (*works)(int), const Examples& examples);
Section Stats(const Examples& examples);
Section Affixes(const Examples& examples);

// One line on what a property func does (the property card shows it under each line).
const char* FuncMeaning(int func);

} // namespace legend
