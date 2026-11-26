// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// The name scan of Live Tables' read hook (lt::NamedRows): which rows of another table's text name a
// stat. Made-up cases first, then the real texts (skills, states, items, ... copies) against an
// independent reading: every cell split out, every quoted name in a formula found by a regex.
//   names_test <itemstatcost.txt> <other table texts...>
#include "../table_model.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace lt;
static int g_fail = 0, g_pass = 0;
static void Expect(bool ok, const std::string& what) {
	if (ok) ++g_pass; else { ++g_fail; printf("FAIL  %s\n", what.c_str()); }
}
static std::string ReadFile(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::vector<size_t> R(std::initializer_list<size_t> l) { return l; }

int main(int argc, char** argv) {
	const std::string h = "name\tstat1\tcalc1\r\n";
	struct Case { const char* what; std::string text; std::vector<size_t> want; };
	const Case cases[] = {
		{ "a whole cell", h + "a\tlt_c\t\r\n", R({1}) },
		{ "the last cell of the text", h + "a\tx\tlt_c", R({1}) },
		{ "stat('x'.accr)", h + "a\t\tstat('lt_c'.accr)*2\r\n", R({1}) },
		{ "'x'.accr without stat()", h + "a\t\t'lt_c'.accr\r\n", R({1}) },
		{ "\"x\" in double quotes", h + "a\t\tstat(\"lt_c\")\r\n", R({1}) },
		{ "inside commas", h + "a\tfoo,lt_c,bar\t\r\n", R({1}) },
		{ "inside parentheses", h + "a\t\tmin(lt_c)\r\n", R({1}) },
		{ "case", h + "a\tLT_C\t\r\n", R({1}) },
		{ "a longer name: lt_cx", h + "a\tlt_cx\tstat('lt_cx'.accr)\r\n", R({}) },
		{ "a longer name before: xlt_c", h + "a\txlt_c\t\r\n", R({}) },
		{ "inside a word", h + "a\tthe lt_c stat\t\r\n", R({}) },
		{ "the header is not a row", "lt_c\tstat1\r\nb\tx\r\n", R({}) },
		{ "rows counted right", h + "a\tx\t\r\nb\ty\t\r\nc\tlt_c\t\r\nd\tlt_c\t\r\n", R({3, 4}) },
		{ "twice in one row: once", h + "a\tlt_c\tstat('lt_c')\r\n", R({1}) },
	};
	for (const Case& c : cases) Expect(NamedRows(c.text, "lt_c") == c.want, c.what);

	// the real texts: every stat name of itemstatcost against an independent reading of each text
	if (argc >= 3) {
		std::vector<std::string> names;
		for (const auto& r : ReadSheet(ReadFile(argv[1])).rows) if (!r[0].empty()) names.push_back(r[0]);
		size_t texts = 0, hits = 0, mismatches = 0;
		for (int i = 2; i < argc; ++i) {
			const std::string text = ReadFile(argv[i]);
			if (text.empty()) continue;
			++texts;
			// independent reading: line by line, cells by tab, then a regex for quoted / delimited use
			std::vector<std::string> lines;
			{ std::stringstream ss(text); std::string l; while (std::getline(ss, l)) lines.push_back(l); }
			for (const std::string& name : names) {
				std::set<size_t> want;
				const std::string esc = std::regex_replace(name, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
				const std::regex re("(^|[\\t\\r'\",(])" + esc + "($|[\\t\\r'\",.)])", std::regex::icase);
				for (size_t l = 1; l < lines.size(); ++l) if (std::regex_search(lines[l], re)) want.insert(l);
				const std::vector<size_t> got = NamedRows(text, name);
				if (std::set<size_t>(got.begin(), got.end()) != want) {
					if (++mismatches <= 5) printf("FAIL  %s: '%s' rows differ (scan %zu, reading %zu)\n", argv[i], name.c_str(), got.size(), want.size());
				}
				hits += got.size();
				// every row where a cell IS the name must be found
				for (size_t l = 1; l < lines.size(); ++l) {
					std::string line = lines[l];
					if (!line.empty() && line.back() == '\r') line.pop_back();
					std::stringstream cs(line); std::string cell; bool eq = false;
					while (std::getline(cs, cell, '\t')) eq = eq || Lower(cell) == Lower(name);
					if (eq && !want.count(l)) { ++mismatches; printf("FAIL  %s row %zu: a cell is '%s' but it was not found\n", argv[i], l, name.c_str()); }
				}
			}
		}
		Expect(mismatches == 0, "real texts: the scan == the independent reading for every stat name");
		printf("names %zu stat names in %zu texts: %zu (name, row) pairs found, %zu differ\n", names.size(), texts, hits, mismatches);
	}
	printf("%s  %d checks passed, %d failed\n", g_fail ? "FAIL" : "ok  ", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
