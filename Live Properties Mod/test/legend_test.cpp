// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The Legend's sections: every list row fits one line, codes differ, texts are plain ASCII, the
// examples from the tables land in the right detail, funcs without a handler offer no Insert.
#include "../legend_core.h"

#include <cstdio>
#include <set>

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

static bool Plain(const std::string& text) {
	for (const char c : text) {
		if ((static_cast<unsigned char>(c) < 0x20 && c != '\n') || static_cast<unsigned char>(c) > 0x7e) {
			return false;
		}
	}
	return true;
}

static void Shape(const char* name, const legend::Section& s) {
	size_t longest = 0;
	bool   filled = true, plain = true, oneLine = true, unique = true;
	for (const auto* part : { &s.how, &s.list }) {
		std::set<std::string> codes;
		for (const legend::Item& item : *part) {
			const size_t row = item.code.size() + 3 + item.meaning.size(); // "code - meaning"
			longest          = row > longest ? row : longest;
			filled  = filled && !item.code.empty() && !item.meaning.empty() && !item.detail.empty();
			plain   = plain && Plain(item.code) && Plain(item.meaning) && Plain(item.detail) && Plain(item.insert);
			oneLine = oneLine && item.meaning.find('\n') == std::string::npos && item.code.find('\n') == std::string::npos;
			unique  = unique && codes.insert(item.code).second;
		}
	}
	Check(!s.how.empty() && !s.list.empty(), std::string(name) + ": has How it works entries and a list");
	Check(longest <= legend::kRowMost, std::string(name) + ": every \"code - meaning\" row fits Mod Book's line (longest " + std::to_string(longest) + ")");
	Check(filled && plain && oneLine, std::string(name) + ": code, meaning and detail filled, plain ASCII, one line");
	Check(unique, std::string(name) + ": codes differ");
}

static bool Works(int func) {
	return (func >= 1 && func <= 25) || func == 36; // as image 6ab3782c: 37 funcs, null 26-35
}

int main() {
	legend::Examples ex { { "func 1", "res-all line 1: func 1 fireresist" }, { "val shift", "maxhp: 8" }, { "pre= / suf=", "pre=135 = Stout" } };
	const legend::Section props = legend::Properties(37, Works, ex);
	const legend::Section stats = legend::Stats(ex);
	const legend::Section affix = legend::Affixes(ex);
	Shape("Properties", props);
	Shape("Stats", stats);
	Shape("Affixes", affix);

	size_t funcs = 0, inserts = 0;
	bool   dead  = true;
	for (const legend::Item& item : props.list) {
		if (item.code.rfind("func ", 0) == 0) {
			++funcs;
			const int n = atoi(item.code.c_str() + 5);
			inserts += item.insert.empty() ? 0 : 1;
			dead = dead && (n == 0 || Works(n) || item.insert.empty());
		}
	}
	Check(funcs == 37, "Properties: one entry per func the game has (37)");
	Check(dead && inserts == 27, "Properties: Insert only for funcs that work (0, 1-25, 36: " + std::to_string(inserts) + ")");

	auto find = [](const std::vector<legend::Item>& list, const std::string& code) -> const legend::Item* {
		for (const legend::Item& item : list) {
			if (item.code == code) {
				return &item;
			}
		}
		return nullptr;
	};
	const legend::Item* f1 = find(props.list, "func 1");
	const legend::Item* vs = find(stats.list, "val shift");
	const legend::Item* pre = find(affix.how, "pre= / suf=");
	Check(f1 != nullptr && f1->detail.find("In your tables: res-all line 1") != std::string::npos && f1->insert == "1", "func 1: example and Insert");
	Check(vs != nullptr && vs->detail.find("In your tables: maxhp: 8") != std::string::npos, "val shift: example from the tables");
	Check(pre != nullptr && pre->detail.find("Stout") != std::string::npos && pre->insert == "pre=", "pre= / suf=: example, Insert pre=");
	const legend::Item* op2 = find(stats.list, "op 2");
	Check(op2 != nullptr && op2->detail.find(">>") != std::string::npos && op2->insert == "2", "op 2: divides (>>), Insert 2");
	Check(std::string(legend::FuncMeaning(26)) == "not in the game's data guide" && std::string(legend::FuncMeaning(21)).find("val") != std::string::npos, "FuncMeaning: 21 names val, 26 has none");

	printf(g_failed == 0 ? "legend: all passed\n" : "legend: %d FAILED\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
