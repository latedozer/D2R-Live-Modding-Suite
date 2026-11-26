// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Export: edits into text tables, and the D2RMM mod (see export_core.h).
#include "export_core.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace exporter {

// ---------------------------------------------------------------- tables

int Tsv::Column(const std::string& name) const {
	for (size_t i = 0; i < headers.size(); ++i) {
		if (headers[i] == name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::string& Tsv::At(size_t row, const std::string& column) {
	static std::string none;
	const int at = Column(column);
	if (at < 0 || row >= rows.size()) {
		none.clear();
		return none;
	}
	if (rows[row].size() < headers.size()) {
		rows[row].resize(headers.size());
	}
	return rows[row][static_cast<size_t>(at)];
}

static std::vector<std::string> Split(const std::string& line, char by) {
	std::vector<std::string> parts;
	size_t                   from = 0;
	while (true) {
		const size_t at = line.find(by, from);
		parts.push_back(line.substr(from, at == std::string::npos ? std::string::npos : at - from));
		if (at == std::string::npos) {
			return parts;
		}
		from = at + 1;
	}
}

static std::vector<std::string> Lines(const std::string& text) {
	std::vector<std::string> lines;
	size_t                   from = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	while (from < text.size()) {
		size_t end = text.find('\n', from);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(from, end - from);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		lines.push_back(line);
		from = end + 1;
	}
	return lines;
}

bool ParseTsv(const std::string& text, Tsv& tsv) {
	tsv = Tsv {};
	const std::vector<std::string> lines = Lines(text);
	if (lines.empty()) {
		return false;
	}
	tsv.headers = Split(lines[0], '\t');
	for (size_t i = 1; i < lines.size(); ++i) {
		if (i + 1 == lines.size() && lines[i].empty()) {
			break; // the file's last line end
		}
		std::vector<std::string> row = Split(lines[i], '\t');
		row.resize(tsv.headers.size() > row.size() ? tsv.headers.size() : row.size());
		tsv.rows.push_back(std::move(row));
	}
	return true;
}

std::string FormatTsv(const Tsv& tsv) {
	std::string out;
	const auto  line = [&](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); ++i) {
            out += (i == 0 ? "" : "\t") + cells[i];
        }
        out += "\r\n";
	};
	line(tsv.headers);
	for (std::vector<std::string> row : tsv.rows) {
		row.resize(tsv.headers.size());
		line(row);
	}
	return out;
}

// ---------------------------------------------------------------- the edit files

bool Edits::Empty() const {
	return props.empty() && propsOff.empty() && madeProps.empty() && stats.empty() && grown.empty() && statsOff.empty() && affixes.empty() && affixesOff.empty() && recipes.rows.empty() && recipesBase.rows.empty();
}

static std::vector<std::vector<std::string>> Records(const std::string& text) {
	std::vector<std::vector<std::string>> out;
	for (const std::string& line : Lines(text)) {
		if (!line.empty() && line[0] != '#') {
			out.push_back(Split(line, '\t'));
		}
	}
	return out;
}
static std::string Cell(const std::string& text) {
	return text == "-" ? std::string() : text;
}

std::vector<std::string> ReadPropertyEdits(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const auto& parts : Records(text)) {
		if (parts.size() != kPropCells + 1 || parts[0].empty()) {
			bad.push_back("property-edits.txt: a line with " + std::to_string(parts.size()) + " cells was skipped");
			continue;
		}
		PropEdit edit;
		edit.code = parts[0];
		for (int i = 0; i < kPropCells; ++i) {
			edit.cells[static_cast<size_t>(i)] = Cell(parts[static_cast<size_t>(i + 1)]);
		}
		edits.props.push_back(edit);
	}
	return bad;
}

// The name a made stat is found by in listMade: its made name, else the row's name (not a filler's).
static std::string MadeName(const StatEdit& s) {
	return !s.name.empty() ? s.name : IsFillerName(s.was) ? std::string() : s.was;
}

void TakeMadeRows(Edits& edits, Report& report) {
	// what the game has this start: Live Tables' grown rows (find) first, then Live Properties' listMade
	struct Row {
		std::string    name;
		int            number = -1, baseNumber = -1;
		const GrownRow* grown = nullptr;
		bool           taken  = false;
	};
	std::vector<Row> stats, props;
	const auto add = [](std::vector<Row>& rows, const std::string& name, int number, int baseNumber, const GrownRow* grown) {
		for (Row& r : rows) {
			if (r.name == name) {
				r.number     = r.number >= 0 ? r.number : number;
				r.baseNumber = r.baseNumber >= 0 ? r.baseNumber : baseNumber;
				r.grown      = r.grown != nullptr ? r.grown : grown;
				return;
			}
		}
		rows.push_back({ name, number, baseNumber, grown });
	};
	const bool tables = edits.liveTables != LiveTables::missing, props_ = edits.liveProps == LiveProps::ok;
	for (const GrownRow& g : edits.grown) {
		if (!tables) {
			break;
		}
		if (g.number < 0 && g.baseNumber < 0) {
			report.problems.push_back(std::string(g.stat ? "itemstatcost: grown stat " : "properties: grown property ") + g.name + ": not in the game this start (Live Tables does not find it); left out");
			continue;
		}
		add(g.stat ? stats : props, g.name, g.number, g.baseNumber, &g);
	}
	if (props_) {
		for (const MadeRow& r : edits.madeStatRows) {
			int base = -1;
			for (const MadeRow& b : edits.madeStatRowsBase) {
				base = b.name == r.name ? b.number : base;
			}
			add(stats, r.name, r.number, base, nullptr);
		}
		for (const MadeRow& r : edits.madePropRows) {
			add(props, r.name, r.number, -1, nullptr);
		}
	}
	const char* why = "neither Live Tables nor Live Properties (API version 2: listMade) is running, so nothing says the game has it";
	const bool  any = tables || props_;
	const auto  find = [](std::vector<Row>& rows, const std::string& name) -> Row* {
		for (Row& r : rows) {
			if (r.name == name) {
				return &r;
			}
		}
		return nullptr;
	};
	const auto fill = [](StatEdit& s, Row& r) {
		s.id     = r.number;
		s.baseId = r.baseNumber;
		s.name   = r.name;
		s.made   = true;
		if (r.grown != nullptr) {
			s.grownCells = r.grown->cells;
		}
		r.taken = true;
	};
	std::vector<StatEdit> kept, unmatched;
	for (StatEdit s : edits.stats) {
		if (!s.made) {
			kept.push_back(s);
			continue;
		}
		const std::string name = MadeName(s);
		const std::string what = "itemstatcost: made stat " + (name.empty() ? "(no name, line " + std::to_string(s.id) + ")" : name);
		if (!any) {
			report.problems.push_back(what + ": left out - " + why);
			continue;
		}
		if (Row* r = name.empty() ? nullptr : find(stats, name); r != nullptr && r->number >= 0) {
			fill(s, *r);
			kept.push_back(s);
		} else {
			unmatched.push_back(s);
		}
	}
	// a line whose name the game no longer has: renamed in Live Tables (its files follow, this line
	// not yet) when an untaken made row sits at the line's number
	for (StatEdit s : unmatched) {
		const std::string name = MadeName(s);
		Row*              at   = nullptr;
		for (Row& r : stats) {
			at = at == nullptr && !r.taken && r.number == s.id ? &r : at;
		}
		if (at != nullptr) {
			report.done.push_back("itemstatcost: made stat " + name + " is " + at->name + " now (renamed): exported as " + at->name);
			fill(s, *at);
			kept.push_back(s);
		} else {
			report.problems.push_back("itemstatcost: made stat " + (name.empty() ? "(no name)" : name) + ": not in the game this start; left out");
		}
	}
	for (Row& r : stats) {
		if (r.taken || r.number < 0) {
			continue;
		}
		if (r.grown != nullptr) { // grown with no stat card edits: its cells as grown
			StatEdit s;
			s.lined = false;
			fill(s, r);
			kept.push_back(s);
		} else {
			report.problems.push_back("itemstatcost: made stat " + r.name + " (" + std::to_string(r.number) + ") is in the game, but stat-edits.txt has no line for it; left out");
		}
	}
	edits.stats = kept;
	// made properties: by code, in the game's row order
	std::vector<std::pair<int, std::string>> order;
	for (const std::string& code : edits.madeProps) {
		Row* r = any ? find(props, code) : nullptr;
		if (!any) {
			report.problems.push_back("properties: made property " + code + ": left out - " + why);
		} else if (r == nullptr || r->number < 0) {
			report.problems.push_back("properties: made property " + code + ": not in the game this start; left out");
		} else {
			r->taken = true;
			order.emplace_back(r->number, code);
		}
	}
	for (Row& r : props) {
		if (!r.taken && r.grown != nullptr && r.number >= 0) {
			order.emplace_back(r.number, r.name);
		}
	}
	edits.madePropCells.clear();
	for (const Row& r : props) {
		if (r.grown != nullptr) {
			edits.madePropCells[r.name] = r.grown->cells;
		}
	}
	std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	edits.madeProps.clear();
	for (const auto& p : order) {
		edits.madeProps.push_back(p.second);
	}
	if (!any) {
		report.problems.push_back(std::string("made stats and properties: none in the export - ") + why);
	}
}

static std::string Trimmed(std::string line) {
	while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
		line.pop_back();
	}
	return line;
}

std::vector<std::string> ReadGrownRows(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const std::string& raw : Lines(text)) {
		const std::string line = Trimmed(raw);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const std::vector<std::string> parts = Split(line, '\t');
		GrownRow                       g;
		g.stat           = parts[0] == "itemstatcost";
		const bool known = g.stat || parts[0] == "properties";
		for (size_t i = 1; i < parts.size(); ++i) {
			const size_t eq = parts[i].find('=');
			if (eq == std::string::npos || eq == 0) {
				continue;
			}
			const std::string column = parts[i].substr(0, eq), value = parts[i].substr(eq + 1);
			if (i <= 2 && (column == "v" || column == "n")) {
				continue; // data versions, the number when made: the game's numbers come from find()
			}
			if (column == (g.stat ? "Stat" : "code")) {
				g.name = value;
			}
			g.cells.emplace_back(column, value);
		}
		if (!known || g.name.empty()) {
			bad.push_back("grown-rows.txt: the line '" + line.substr(0, 60) + "' was skipped");
			continue;
		}
		edits.grown.push_back(g);
	}
	return bad;
}

std::vector<std::string> ReadDisabledRows(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const std::string& raw : Lines(text)) {
		const std::string line = Trimmed(raw);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const std::vector<std::string> parts = Split(line, '\t');
		if (parts.size() != 3 || parts[0] != "itemstatcost" || parts[1].rfind("v=", 0) != 0 || parts[2].empty()) {
			bad.push_back("disabled-rows.txt: the line '" + line.substr(0, 60) + "' was skipped");
			continue;
		}
		edits.statsOff.push_back({ parts[2] });
	}
	return bad;
}

std::vector<std::string> ReadAffixOff(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	static const char* const words[] = { "suffix ", "prefix ", "automagic " };
	for (const std::string& raw : Lines(text)) {
		const std::string line = Trimmed(raw);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		bool ok = false;
		for (int kind = 0; kind < 3 && !ok; ++kind) {
			const std::string word = words[kind];
			const std::string n    = line.rfind(word, 0) == 0 ? line.substr(word.size()) : std::string();
			if (!n.empty() && n.size() <= 5 && n.find_first_not_of("0123456789") == std::string::npos) {
				edits.affixesOff.push_back({ kind, static_cast<size_t>(atoi(n.c_str())), {} });
				ok = true;
			}
		}
		if (!ok) {
			bad.push_back("affix-off.txt: the line '" + line.substr(0, 60) + "' was skipped");
		}
	}
	return bad;
}

std::string CouldNotCarry(const Report& report) {
	std::string line;
	for (const std::string& p : report.problems) {
		line += (line.empty() ? "" : "; ") + p;
	}
	return "could not carry: " + (line.empty() ? std::string("nothing") : line);
}

void ReadMadeProperties(const std::string& text, Edits& edits) {
	size_t from = 0;
	while (from < text.size()) {
		size_t end = text.find('\n', from);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(from, end - from);
		from             = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::string code = line.substr(0, line.find('\t'));
		while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) {
			code.pop_back();
		}
		if (!code.empty() && std::find(edits.madeProps.begin(), edits.madeProps.end(), code) == edits.madeProps.end()) {
			edits.madeProps.push_back(code);
		}
	}
}

void ReadPropertyOff(const std::string& text, Edits& edits) {
	size_t from = 0;
	while (from < text.size()) {
		size_t end = text.find('\n', from);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(from, end - from);
		from             = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty() || line == "#" || line.rfind("# ", 0) == 0) {
			continue;
		}
		if (std::find(edits.propsOff.begin(), edits.propsOff.end(), line) == edits.propsOff.end()) {
			edits.propsOff.push_back(line);
		}
	}
}

std::vector<std::string> ReadStatEdits(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const auto& parts : Records(text)) {
		if (parts.size() != kStatCells + 3 || parts[0].empty()) {
			bad.push_back("stat-edits.txt: a line with " + std::to_string(parts.size()) + " cells was skipped");
			continue;
		}
		StatEdit edit;
		edit.id     = atoi(parts[0].c_str());
		edit.baseId = edit.id;
		for (int i = 0; i < kStatCells; ++i) {
			edit.cells[static_cast<size_t>(i)] = Cell(parts[static_cast<size_t>(i + 1)]);
		}
		edit.name = Cell(parts[kStatCells + 1]);
		edit.was  = Cell(parts[kStatCells + 2]);
		edit.made = !edit.name.empty() || edit.was.empty() || IsFillerName(edit.was); // the plugin's made name is the sure sign
		edits.stats.push_back(edit);
	}
	return bad;
}

std::vector<std::string> ReadAffixEdits(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const auto& parts : Records(text)) {
		const int kind = parts.empty() ? -1 : parts[0] == "suffix" ? kSuffix : parts[0] == "prefix" ? kPrefix : parts[0] == "automagic" ? kAutomagic : -1;
		if (parts.size() != kAffixCells + 3 || kind < 0) {
			bad.push_back("affix-edits.txt: a line with " + std::to_string(parts.size()) + " cells was skipped");
			continue;
		}
		AffixEdit edit;
		edit.kind  = kind;
		edit.index = static_cast<size_t>(strtoul(parts[1].c_str(), nullptr, 10));
		edit.was   = Cell(parts[2]);
		for (int i = 0; i < kAffixCells; ++i) {
			edit.cells[static_cast<size_t>(i)] = Cell(parts[static_cast<size_t>(i + 3)]);
		}
		edits.affixes.push_back(edit);
	}
	return bad;
}

void ReadStringKeys(const std::string& text, Edits& edits) {
	for (const auto& parts : Records(text)) {
		if (parts.size() >= 2 && !parts[0].empty() && !parts[1].empty()) {
			edits.stringKeys[atol(parts[0].c_str())] = parts[1];
		}
	}
}

// ---------------------------------------------------------------- into the tables

const char* const kStatColumns[kStatCells] = { "Signed", "Send Bits", "Save Bits", "Save Add", "ValShift", "descpriority", "descfunc", "descval", "descstrpos", "descstrneg", "op",
	"op param", "op base", "op stat1", "op stat2", "op stat3" };
const char* const kAffixColumns[kAffixCells] = { "Name", "spawnable", "rare", "level", "maxlevel", "levelreq", "frequency", "group", "classspecific", "class", "classlevelreq",
	"mod1code", "mod1param", "mod1min", "mod1max", "mod2code", "mod2param", "mod2min", "mod2max", "mod3code", "mod3param", "mod3min", "mod3max", "itype1", "itype2", "itype3",
	"itype4", "itype5", "itype6", "itype7", "etype1", "etype2", "etype3", "etype4", "etype5" };
const char* const kStatOffColumns[10] = { "op", "op param", "op base", "op stat1", "op stat2", "op stat3", "descfunc", "dgrp", "itemevent1", "itemevent2" };
constexpr int kStrPosCell = 8, kStrNegCell = 9, kClassCells[] = { 8, 9, 10 };

static bool IsNumber(const std::string& text) {
	if (text.empty()) {
		return false;
	}
	char* end = nullptr;
	strtol(text.c_str(), &end, 10);
	return *end == '\0';
}
// A number as the game's files write it: empty for 0.
static std::string Plain(const std::string& cell) {
	return cell.empty() || cell == "0" ? std::string() : cell;
}

// A cell by column name in any case (Live Tables' "column=value" cells), or nullptr (no such column).
static std::string* Cell(Tsv& t, size_t row, const std::string& column) {
	for (size_t c = 0; c < t.headers.size(); ++c) {
		if (t.headers[c].size() == column.size() && std::equal(column.begin(), column.end(), t.headers[c].begin(), [](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); })) {
			if (t.rows[row].size() < t.headers.size()) {
				t.rows[row].resize(t.headers.size());
			}
			return &t.rows[row][c];
		}
	}
	return nullptr;
}

void ApplyProperties(Tsv& properties, const Edits& edits, Report& report) {
	const int code = properties.Column("code");
	while (code >= 0 && !properties.rows.empty() && properties.rows.back()[static_cast<size_t>(code)].rfind("live_free_p", 0) == 0) {
		properties.rows.pop_back(); // Live Properties' spare rows: empty
	}
	// made properties: new rows at the end, under their codes (their cells: the edits below)
	const int eol = properties.Column("*eol");
	for (const std::string& made : edits.madeProps) {
		bool there = false;
		for (size_t r = 0; r < properties.rows.size() && code >= 0 && !there; ++r) {
			there = properties.rows[r][static_cast<size_t>(code)] == made;
		}
		if (code < 0 || there) {
			continue;
		}
		std::vector<std::string> row(properties.headers.size());
		row[static_cast<size_t>(code)] = made;
		if (eol >= 0) {
			row[static_cast<size_t>(eol)] = "0";
		}
		properties.rows.push_back(row);
		const auto grown = edits.madePropCells.find(made);
		if (grown != edits.madePropCells.end()) {
			for (const auto& [column, value] : grown->second) {
				if (std::string* cell = Cell(properties, properties.rows.size() - 1, column); cell != nullptr) {
					*cell = value;
				}
			}
		}
		report.done.push_back("properties: " + made + (grown != edits.madePropCells.end() ? " (made, grown in Live Tables)" : " (made)"));
	}
	for (const PropEdit& edit : edits.props) {
		size_t row = properties.rows.size();
		for (size_t r = 0; r < properties.rows.size() && code >= 0; ++r) {
			if (properties.rows[r][static_cast<size_t>(code)] == edit.code) {
				row = r;
				break;
			}
		}
		if (row == properties.rows.size()) {
			report.problems.push_back("properties: no row '" + edit.code + "'; that edit is not in the export");
			continue;
		}
		static const char* const kinds[] = { "func", "stat", "set", "val" };
		for (int line = 0; line < 7; ++line) {
			for (int k = 0; k < 4; ++k) {
				const std::string& cell = edit.cells[static_cast<size_t>(line * 4 + k)];
				properties.At(row, kinds[k] + std::to_string(line + 1)) = k == 1 ? cell : Plain(cell);
			}
		}
		report.done.push_back("properties: " + edit.code);
	}
	// switched off: the row stays (items, affixes and recipes that name it still read), it does nothing
	for (const std::string& off : edits.propsOff) {
		size_t row = properties.rows.size();
		for (size_t r = 0; r < properties.rows.size() && code >= 0; ++r) {
			if (properties.rows[r][static_cast<size_t>(code)] == off) {
				row = r;
				break;
			}
		}
		if (row == properties.rows.size()) {
			report.problems.push_back("properties: no row '" + off + "' to switch off; left out");
			continue;
		}
		for (int line = 1; line <= 7; ++line) {
			properties.At(row, "func" + std::to_string(line)).clear();
		}
		report.done.push_back("properties: " + off + " switched off");
	}
}

void ApplyStats(Tsv& isc, const Edits& edits, bool base, Report& report) {
	const std::string table = base ? "base\\itemstatcost" : "itemstatcost";
	const int         name  = isc.Column("Stat"), id = isc.Column("*ID"), eol = isc.Column("*eol");
	if (name < 0) {
		report.problems.push_back(table + ": the table has no Stat column; no stat edit is in the export");
		return;
	}
	while (!isc.rows.empty() && isc.rows.back()[static_cast<size_t>(name)].rfind("live_free_", 0) == 0) {
		isc.rows.pop_back(); // Live Properties' spare rows: empty, nothing points at them
	}
	// the stat number -> its name in this table (a stat cell may hold a number)
	const auto nameOf = [&](const std::string& cell) -> std::string {
		if (!IsNumber(cell)) {
			return cell;
		}
		const long n = atol(cell.c_str());
		return n >= 0 && static_cast<size_t>(n) < isc.rows.size() ? isc.rows[static_cast<size_t>(n)][static_cast<size_t>(name)] : cell;
	};
	const auto write = [&](const StatEdit& edit, size_t row, int number) {
		for (int i = 0; i < kStatCells; ++i) {
			const std::string& cell = edit.cells[static_cast<size_t>(i)];
			std::string&       out  = isc.At(row, kStatColumns[i]);
			if (i == kStrPosCell || i == kStrNegCell) {
				if (cell.empty()) {
					out.clear();
				} else if (const auto key = edits.stringKeys.find(atol(cell.c_str())); key != edits.stringKeys.end()) {
					out = key->second;
				} else if (!IsNumber(cell)) {
					out = cell;
				} else if (edits.noString.count(atol(cell.c_str())) != 0) {
					// the game's "no string" (an empty or unknown key): the table's own cell is what gave it
				} else {
					report.problems.push_back(table + ": stat " + std::to_string(number) + " " + kStatColumns[i] + ": string number " + cell + " has no known key; the table's own cell is kept");
				}
			} else if (i >= 12) {
				out = nameOf(cell);
			} else {
				out = Plain(cell);
			}
		}
	};
	// the game's own rows, changed
	for (const StatEdit& edit : edits.stats) {
		if (edit.made || edit.id < 0) {
			continue;
		}
		const size_t row = static_cast<size_t>(edit.id);
		if (row >= isc.rows.size()) {
			report.problems.push_back(table + ": stat " + std::to_string(edit.id) + " (" + edit.was + ") is past the table's end; that edit is not in the export");
			continue;
		}
		if (isc.At(row, "Stat") != edit.was) {
			report.problems.push_back(table + ": stat " + std::to_string(edit.id) + " is '" + isc.At(row, "Stat") + "' in this table, not '" + edit.was + "'; that edit is not in the export");
			continue;
		}
		write(edit, row, edit.id);
		report.done.push_back(table + ": stat " + std::to_string(edit.id) + " " + edit.was);
	}
	// made stats, in number order: each the next row (or its own row, already named so)
	std::vector<const StatEdit*> made;
	for (const StatEdit& edit : edits.stats) {
		if (edit.made) {
			made.push_back(&edit);
		}
	}
	std::stable_sort(made.begin(), made.end(), [&](const StatEdit* a, const StatEdit* b) { return (base ? a->baseId : a->id) < (base ? b->baseId : b->id); });
	for (const StatEdit* edit : made) {
		const int number = base ? edit->baseId : edit->id;
		if (number < 0) {
			report.problems.push_back(table + ": made stat " + edit->name + ": not in these tables in the game this start; left out of them");
			continue;
		}
		const size_t row = static_cast<size_t>(number);
		if (row < isc.rows.size()) {
			if (isc.At(row, "Stat") != edit->name) {
				report.problems.push_back(table + ": made stat " + edit->name + " is " + std::to_string(number) + " in the game, but that row is '" + isc.At(row, "Stat") + "' in this table; left out");
				continue;
			}
		} else if (row == isc.rows.size()) {
			std::vector<std::string> fresh(isc.headers.size());
			fresh[static_cast<size_t>(name)] = edit->name;
			if (id >= 0) {
				fresh[static_cast<size_t>(id)] = std::to_string(number);
			}
			if (eol >= 0) {
				fresh[static_cast<size_t>(eol)] = "0";
			}
			isc.rows.push_back(fresh);
		} else {
			report.problems.push_back(table + ": made stat " + edit->name + " is " + std::to_string(number) + " in the game, but this table's next row is " + std::to_string(isc.rows.size())
			                          + "; left out (Live Export writes no filler rows)");
			continue;
		}
		for (const auto& [column, value] : edit->grownCells) {
			if (std::string* cell = Cell(isc, row, column); cell != nullptr && column != "*ID") {
				*cell = value;
			}
		}
		if (edit->lined) {
			write(*edit, row, number);
		}
		report.done.push_back(table + ": stat " + std::to_string(number) + " " + edit->name + " (made" + (edit->grownCells.empty() ? "" : ", grown in Live Tables") + ")");
	}
	// disabled stats (Live Tables): no effect, still saved - the cells Live Tables makes inert
	for (const StatOff& off : edits.statsOff) {
		size_t row = isc.rows.size();
		for (size_t r = 0; r < isc.rows.size() && row == isc.rows.size(); ++r) {
			row = isc.rows[r][static_cast<size_t>(name)] == off.name ? r : row;
		}
		if (row == isc.rows.size()) {
			report.problems.push_back(table + ": disabled stat " + off.name + ": no such row in this table; left out");
			continue;
		}
		for (const char* column : kStatOffColumns) {
			if (std::string* cell = Cell(isc, row, column); cell != nullptr) {
				cell->clear();
			}
		}
		report.done.push_back(table + ": stat " + std::to_string(row) + " " + off.name + " disabled");
	}
}

void ApplyAffixes(Tsv& table, AffixKind kind, const Edits& edits, const std::function<bool(const std::string&)>& isKey, std::vector<NewString>& strings, Report& report) {
	static const char* const kinds[] = { "magicsuffix", "magicprefix", "automagic" };
	const std::string        what    = kinds[kind];
	for (const AffixEdit& edit : edits.affixes) {
		if (edit.kind != kind) {
			continue;
		}
		const std::string label = what + " " + std::to_string(edit.index);
		if (edit.index >= table.rows.size()) {
			report.problems.push_back(label + ": the table has no such row; that edit is not in the export");
			continue;
		}
		std::string& name = table.At(edit.index, "Name");
		if (name != edit.was) {
			report.problems.push_back(label + " is '" + name + "' in this table, not '" + edit.was + "'; that edit is not in the export");
			continue;
		}
		for (int i = 1; i < kAffixCells; ++i) {
			bool classCell = false;
			for (const int c : kClassCells) {
				classCell = classCell || c == i;
			}
			if (classCell) {
				if (!edit.cells[static_cast<size_t>(i)].empty() && edit.was.empty()) {
					report.problems.push_back(label + ": " + kAffixColumns[i] + " is not exported (the game keeps it as a number, the table as a class code)");
				}
				continue;
			}
			const std::string& cell = edit.cells[static_cast<size_t>(i)];
			table.At(edit.index, kAffixColumns[i]) = cell.size() > 1 && cell[0] == '#' ? std::string() : Plain(cell);
		}
		const std::string& words = edit.cells[0];
		if (words.empty() || isKey(words)) {
			name = words;
		} else {
			NewString string { "live_" + what + "_" + std::to_string(edit.index), words };
			name = string.key;
			strings.push_back(string);
		}
		report.done.push_back(label + (edit.was.empty() ? " (made): " : ": ") + words);
	}
	// disabled (Live Properties' affix-off.txt): it rolls on no drop and no recipe can force it
	for (const AffixOff& off : edits.affixesOff) {
		if (off.kind != kind) {
			continue;
		}
		const std::string label = what + " " + std::to_string(off.index);
		bool              edited = false;
		for (const AffixEdit& e : edits.affixes) {
			edited = edited || (e.kind == kind && e.index == off.index);
		}
		if (off.index >= table.rows.size() || (!edited && !off.was.empty() && table.At(off.index, "Name") != off.was)) {
			report.problems.push_back(label + " disabled: the row is not '" + off.was + "' in this table; left out");
			continue;
		}
		table.At(off.index, "spawnable") = "0";
		table.At(off.index, "frequency") = "0";
		report.done.push_back(label + " " + table.At(off.index, "Name") + " disabled (spawnable 0, frequency 0)");
	}
}

// The cube list into the cube's table: the recipes file's rows, column by column name.
bool ApplyCube(Tsv& cube, const Tsv& recipes, Report& report, const char* which) {
	if (recipes.rows.empty()) {
		return false;
	}
	std::vector<std::vector<std::string>> rows;
	for (size_t r = 0; r < recipes.rows.size(); ++r) {
		std::vector<std::string> row(cube.headers.size());
		for (size_t c = 0; c < cube.headers.size(); ++c) {
			const int from = recipes.Column(cube.headers[c]);
			row[c]         = from >= 0 && static_cast<size_t>(from) < recipes.rows[r].size() ? recipes.rows[r][static_cast<size_t>(from)] : "";
		}
		rows.push_back(row);
	}
	cube.rows = rows;
	report.done.push_back(std::string(which) + ": the cube list (" + std::to_string(rows.size()) + " recipes)");
	return true;
}

// ---------------------------------------------------------------- Exalted Items' rows

bool IsExalted(const std::string& description) {
	return description.compare(0, 6, "[xlt] ") == 0;
}

static std::string K(const std::string& cell) {
	size_t from = cell.find_first_not_of(" \t"), to = cell.find_last_not_of(" \t");
	std::string k = from == std::string::npos ? std::string() : cell.substr(from, to - from + 1);
	if (k.size() >= 2 && k.front() == '"' && k.back() == '"') {
		k = k.substr(1, k.size() - 2);
	}
	for (char& c : k) {
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	}
	return k;
}

std::string ExaltedKey(const std::string& output, const std::string& input1) {
	const std::string out = K(output), in = K(input1), code = in.substr(0, in.find(','));
	if (out.find("exc") != std::string::npos) {
		return "upgrade:" + in;
	}
	return (out.find("eli") != std::string::npos ? "downgrade:" : "restore:") + code;
}

const ActionButton kExportButtons[3] = {
	{ "Mpq", "The whole mod (.mpq)", "$FontColorGreen", 2 },
	{ "Plain", ".mpq without Exalted copies", "$FontColorLightTeal", 2 },
	{ "D2rmm", "A D2RMM mod (.ts + .json)", "$FontColorGreen", 2 },
};

bool LabelFits(const std::string& text, int slots) {
	static const size_t most[] = { 12, 28, 36, 38 };
	return text.size() <= most[slots < 1 ? 0 : slots > 4 ? 3 : slots - 1];
}

bool IsFillerName(const std::string& name) {
	return name.rfind("live_free_", 0) == 0 || name.rfind("xlt_unused_", 0) == 0 || name.rfind("live_unused_", 0) == 0;
}

bool ReadExaltedIds(const std::string& text, ExaltedIds& ids, std::string& why) {
	ExaltedIds out;
	bool       layout = false, marker = false;
	size_t     lineNo = 0, from = 0;
	while (from <= text.size()) {
		size_t end = text.find('\n', from);
		end        = end == std::string::npos ? text.size() : end;
		std::string line;
		for (size_t i = from; i < end; ++i) {
			if (text[i] != '\r' && text[i] != ' ' && text[i] != '\t') {
				line += text[i];
			}
		}
		from = end + 1;
		++lineNo;
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const size_t      eq    = line.find('=');
		const std::string key   = line.substr(0, eq), value = eq == std::string::npos ? std::string() : line.substr(eq + 1);
		const bool        isNum = !value.empty() && value.size() <= 4 && value.find_first_not_of("0123456789") == std::string::npos;
		const int         n     = isNum ? atoi(value.c_str()) : -1;
		bool              ok    = true;
		if (key == "layout") {
			ok         = value == "packed" || value == "legacy";
			out.legacy = value == "legacy";
			layout     = ok;
		} else if (key == "marker" || key.rfind("roll.", 0) == 0) {
			ok     = isNum;
			marker = marker || (ok && key == "marker");
			out.own.push_back(n);
		} else if (key == "free") {
			ok = isNum;
			out.free.push_back(n);
		} else if (key.rfind("made.", 0) == 0 && key.size() > 5) {
			ok = isNum;
			out.made[key.substr(5)] = n;
		} else {
			ok = false;
		}
		if (!ok) {
			why = "line " + std::to_string(lineNo) + " ('" + line + "') is not understood";
			return false;
		}
	}
	if (!layout || !marker) {
		why = layout ? "it has no marker line" : "it has no layout line";
		return false;
	}
	ids = out;
	return true;
}

void MarkMadeStats(Edits& edits, const ExaltedIds* ids) {
	for (StatEdit& s : edits.stats) {
		const auto named = ids != nullptr ? ids->made.find(s.was) : std::map<std::string, int>::const_iterator();
		s.made           = !s.name.empty() || s.was.empty() || IsFillerName(s.was) || (ids != nullptr && named != ids->made.end() && named->second == s.id);
	}
}

void LeaveOutExaltedRows(Edits& edits, Report& report, const ExaltedIds* ids) {
	MarkMadeStats(edits, ids);
	std::vector<StatEdit> stats;
	for (const StatEdit& s : edits.stats) {
		const bool own = ids != nullptr && std::find(ids->own.begin(), ids->own.end(), s.id) != ids->own.end();
		if ((s.was.rfind("xlt_", 0) == 0 && s.was.rfind("xlt_unused_", 0) != 0) || own) {
			report.problems.push_back("itemstatcost: stat " + std::to_string(s.id) + " " + s.was + ": not carried, Exalted Items makes this row");
		} else if (ids == nullptr && s.made && s.id > 486) {
			report.problems.push_back("itemstatcost: made stat " + std::to_string(s.id) + ": not carried, Exalted Items' own stats start at 487");
		} else {
			stats.push_back(s);
		}
	}
	edits.stats = stats;
	std::vector<PropEdit> props;
	for (const PropEdit& p : edits.props) {
		if (p.code.rfind("xlt-", 0) == 0) {
			report.problems.push_back("properties: " + p.code + ": not carried, Exalted Items makes this row");
		} else {
			props.push_back(p);
		}
	}
	edits.props = props;
	std::vector<std::string> made;
	for (const std::string& code : edits.madeProps) {
		if (code.rfind("xlt-", 0) != 0) {
			made.push_back(code);
		}
	}
	edits.madeProps = made;
	std::vector<std::string> off;
	for (const std::string& code : edits.propsOff) {
		if (code.rfind("xlt-", 0) == 0) {
			report.problems.push_back("properties: " + code + " switched off: not carried, Exalted Items makes this row");
		} else {
			off.push_back(code);
		}
	}
	edits.propsOff = off;
}

// The cells of a row of `t` at the cube table's columns, joined (to compare whole rows).
static std::string RowKey(const Tsv& t, size_t r, const std::vector<std::string>& columns) {
	std::string k;
	for (const std::string& c : columns) {
		const int at = t.Column(c);
		k += (at >= 0 && static_cast<size_t>(at) < t.rows[r].size() ? t.rows[r][static_cast<size_t>(at)] : "") + "\t";
	}
	return k;
}
static std::string Get(const Tsv& t, size_t r, const char* column) {
	const int at = t.Column(column);
	return at >= 0 && static_cast<size_t>(at) < t.rows[r].size() ? t.rows[r][static_cast<size_t>(at)] : "";
}

bool ApplyCubePlain(Tsv& cube, const Tsv& recipes, const Tsv& madeAgainst, Report& report, const char* which) {
	if (recipes.rows.empty()) {
		return false;
	}
	std::vector<std::string> columns;
	for (const std::string& h : madeAgainst.headers) {
		if (!h.empty() && h[0] != '*') {
			columns.push_back(h);
		}
	}
	std::multiset<std::string> before;
	for (size_t r = 0; r < madeAgainst.rows.size(); ++r) {
		before.insert(RowKey(madeAgainst, r, columns));
	}
	Tsv kept;
	kept.headers = recipes.headers;
	std::set<std::string> keys; // the Exalted Items keys the plain mod brings
	size_t changed = 0, untouched = 0, removed = 0;
	for (size_t r = 0; r < recipes.rows.size(); ++r) {
		if (!IsExalted(Get(recipes, r, "description"))) {
			kept.rows.push_back(recipes.rows[r]);
			continue;
		}
		const auto same = before.find(RowKey(recipes, r, columns));
		if (same != before.end()) {
			before.erase(same); // Exalted Items makes it again: left out
			++untouched;
		} else {
			kept.rows.push_back(recipes.rows[r]);
			keys.insert(ExaltedKey(Get(recipes, r, "output"), Get(recipes, r, "input 1")));
			++changed;
		}
	}
	// the Exalted Items rows the user took out: written with enabled 0, unless a changed one stands for them
	std::multiset<std::string> inList;
	for (size_t r = 0; r < recipes.rows.size(); ++r) {
		inList.insert(RowKey(recipes, r, columns));
	}
	for (size_t r = 0; r < madeAgainst.rows.size(); ++r) {
		if (!IsExalted(Get(madeAgainst, r, "description"))) {
			continue;
		}
		const auto there = inList.find(RowKey(madeAgainst, r, columns));
		if (there != inList.end()) {
			inList.erase(there);
			continue;
		}
		const std::string key = ExaltedKey(Get(madeAgainst, r, "output"), Get(madeAgainst, r, "input 1"));
		if (keys.count(key) != 0) {
			continue;
		}
		std::vector<std::string> row(recipes.headers.size());
		for (size_t c = 0; c < recipes.headers.size(); ++c) {
			const int from = madeAgainst.Column(recipes.headers[c]);
			row[c]         = from >= 0 && static_cast<size_t>(from) < madeAgainst.rows[r].size() ? madeAgainst.rows[r][static_cast<size_t>(from)] : "";
		}
		const int enabled = recipes.Column("enabled");
		if (enabled >= 0) {
			row[static_cast<size_t>(enabled)] = "0";
		}
		kept.rows.push_back(row);
		keys.insert(key);
		++removed;
	}
	ApplyCube(cube, kept, report, which);
	report.done.push_back(std::string(which) + ": Exalted Items' recipes: " + std::to_string(changed) + " changed by you kept, " + std::to_string(removed) + " removed by you (written with enabled 0), "
	                      + std::to_string(untouched) + " left to Exalted Items");
	return true;
}

// ---------------------------------------------------------------- strings files

static std::string Json(const std::string& text) {
	std::string out = "\"";
	for (const char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char code[8];
				snprintf(code, sizeof(code), "\\u%04x", c);
				out += code;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

long HighestStringId(const std::string& json) {
	long        most = 0;
	const char* key  = "\"id\"";
	for (size_t at = json.find(key); at != std::string::npos; at = json.find(key, at + 4)) {
		size_t from = json.find(':', at);
		if (from == std::string::npos) {
			break;
		}
		const long id = strtol(json.c_str() + from + 1, nullptr, 10);
		most          = id > most ? id : most;
	}
	return most;
}

static const char* const kLanguages[] = { "enUS", "zhTW", "deDE", "esES", "frFR", "itIT", "koKR", "plPL", "esMX", "jaJP", "ptBR", "ruRU", "zhCN" };

std::string AddStrings(const std::string& json, const std::vector<NewString>& strings, long nextId) {
	if (strings.empty()) {
		return json;
	}
	const size_t end = json.rfind(']');
	if (end == std::string::npos) {
		return json;
	}
	std::string add;
	for (const NewString& s : strings) {
		add += std::string(",\n  {\"id\":") + std::to_string(nextId++) + ",\"Key\":" + Json(s.key);
		for (const char* language : kLanguages) {
			add += std::string(",\"") + language + "\":" + Json(s.text);
		}
		add += "}";
	}
	size_t last = end;
	while (last > 0 && (json[last - 1] == ' ' || json[last - 1] == '\n' || json[last - 1] == '\r' || json[last - 1] == '\t')) {
		--last;
	}
	const bool empty = last > 0 && json[last - 1] == '[';
	return json.substr(0, last) + (empty ? add.substr(1) : add) + "\n" + json.substr(end);
}

// ---------------------------------------------------------------- the D2RMM mod

std::string D2rmmJson(const std::string& name, const std::string& version, const std::string& description) {
	return "{\n  \"name\": " + Json(name) + ",\n  \"description\": " + Json(description) + ",\n  \"author\": \"Live Suite (Live Export)\",\n  \"version\": " + Json(version) + "\n}\n";
}

static std::string Object(const std::vector<std::pair<std::string, std::string>>& cells) {
	std::string out = "{";
	for (size_t i = 0; i < cells.size(); ++i) {
		out += (i == 0 ? "" : ", ") + Json(cells[i].first) + ": " + Json(cells[i].second);
	}
	return out + "}";
}

// What a cube list adds to and takes out of the list it was made against (whole rows, compared
// on the cube table's columns).
static void CubeDiff(const Tsv& base, const Tsv& recipes, std::vector<std::string>& columns, std::vector<std::vector<std::string>>& added, std::vector<std::vector<std::string>>& removed) {
	columns.clear();
	for (const std::string& h : base.headers) {
		if (!h.empty() && h[0] != '*') {
			columns.push_back(h);
		}
	}
	const auto key = [&](const Tsv& t, size_t r) {
		std::string k;
		for (const std::string& c : columns) {
			const int at = t.Column(c);
			k += (at >= 0 && static_cast<size_t>(at) < t.rows[r].size() ? t.rows[r][static_cast<size_t>(at)] : "") + "\t";
		}
		return k;
	};
	std::multimap<std::string, size_t> inBase, inList;
	for (size_t r = 0; r < base.rows.size(); ++r) {
		inBase.emplace(key(base, r), r);
	}
	for (size_t r = 0; r < recipes.rows.size(); ++r) {
		inList.emplace(key(recipes, r), r);
	}
	std::multimap<std::string, size_t> left = inBase;
	for (size_t r = 0; r < recipes.rows.size(); ++r) {
		const auto found = left.find(key(recipes, r));
		if (found != left.end()) {
			left.erase(found);
		} else {
			std::vector<std::string> row;
			for (const std::string& c : columns) {
				const int at = recipes.Column(c);
				row.push_back(at >= 0 && static_cast<size_t>(at) < recipes.rows[r].size() ? recipes.rows[r][static_cast<size_t>(at)] : "");
			}
			added.push_back(row);
		}
	}
	for (const auto& [k, r] : left) {
		std::vector<std::string> row;
		for (const std::string& c : columns) {
			const int at = base.Column(c);
			row.push_back(at >= 0 && static_cast<size_t>(at) < base.rows[r].size() ? base.rows[r][static_cast<size_t>(at)] : "");
		}
		removed.push_back(row);
	}
}

static std::string Rows(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) {
	std::string out = "[";
	for (size_t r = 0; r < rows.size(); ++r) {
		std::vector<std::pair<std::string, std::string>> cells;
		for (size_t c = 0; c < columns.size(); ++c) {
			if (!rows[r][c].empty()) {
				cells.emplace_back(columns[c], rows[r][c]);
			}
		}
		out += (r == 0 ? "\n    " : ",\n    ") + Object(cells);
	}
	return out + (rows.empty() ? "]" : "\n  ]");
}

static const char* const kTsRuntime = R"TS(
// ---------------------------------------------------------------------------------------------
// Everything below works on the tables D2RMM hands this mod (the game's, changed by the mods
// before this one). Rows are found by their code / name / number and only the cells the Live
// Suite changed are written, so the other mods' changes to a row stay.

// Cells into a row, the column names matched in any case (Live Tables writes them as typed).
function put(table: TSVData, row: TSVDataRow, cells: { [column: string]: string }): void {
  for (const [column, value] of Object.entries(cells)) {
    const header = table.headers.find((h) => h.toLowerCase() === column.toLowerCase());
    if (header !== undefined) {
      row[header] = value;
    }
  }
}

function edit(path: string, change: (table: TSVData) => void): void {
  const table = D2RMM.readTsv(path);
  if (!table) {
    return;
  }
  change(table);
  D2RMM.writeTsv(path, table);
}

// itemstatcost.txt: by number (a stat's number is its row; saved items keep numbers). A stat made
// in the game (Live Properties) goes right after the table's last row, at the number it had in the
// game, or is found at that number under its name. This is checked before anything is written: if
// the table here does not fit (another mod added or took out stats), the mod stops and changes
// nothing, as an item holding the stat would read as another one. It never writes filler rows.
const statsTable = 'global\\excel\\itemstatcost.txt';
const freeRow = (row: TSVDataRow) => (row.Stat ?? '').startsWith('live_free_'); // Live Properties' spare rows at the end: empty
const madeStats = DATA.stats.filter((e) => e.made).sort((a, b) => a.id - b.id);
if (DATA.stats.length > 0) {
  const table = D2RMM.readTsv(statsTable);
  if (table) {
    let end = table.rows.length;
    while (end > 0 && freeRow(table.rows[end - 1])) {
      end--;
    }
    for (const e of DATA.stats) {
      if (!e.made && (e.id >= end || table.rows[e.id].Stat !== e.was)) {
        throw new Error(`Live Export: stat ${e.id} is '${table.rows[e.id]?.Stat ?? 'past the end'}' here, not '${e.was}' - another mod uses that number. Nothing is changed.`);
      }
    }
    for (const e of madeStats) {
      if (e.id < end && table.rows[e.id].Stat !== e.cells.Stat) {
        throw new Error(`Live Export: the made stat ${e.cells.Stat} is ${e.id} in the game, but stat ${e.id} is '${table.rows[e.id].Stat}' here - another mod uses that number. Saved items with it would read wrong, so nothing is changed.`);
      }
      if (e.id > end) {
        throw new Error(`Live Export: the made stat ${e.cells.Stat} is ${e.id} in the game, but the stat table here ends at ${end - 1} (other mods changed it). No filler rows are written, so nothing is changed.`);
      }
      end = e.id === end ? end + 1 : end;
    }
  }
}

// properties.txt: by code
edit('global\\excel\\properties.txt', (table) => {
  // made properties: new rows at the end, under their codes
  for (const made of DATA.madeProperties) {
    if (!table.rows.find((r) => r.code === made.code)) {
      const row: TSVDataRow = {};
      table.headers.forEach((h) => (row[h] = ''));
      row.code = made.code;
      if ('*eol' in row) {
        row['*eol'] = '0';
      }
      put(table, row, made.cells); // a row Live Tables grew: its cells as grown
      table.rows.push(row);
    }
  }
  for (const e of DATA.properties) {
    const row = table.rows.find((r) => r.code === e.code);
    if (!row) {
      console.warn(`Live Export: properties.txt has no row '${e.code}'; that change is left out.`);
      continue;
    }
    Object.assign(row, e.cells);
  }
  // switched off: the row stays (items, affixes and recipes that name it still read), it does nothing
  for (const code of DATA.propertiesOff) {
    const row = table.rows.find((r) => r.code === code);
    if (!row) {
      console.warn(`Live Export: properties.txt has no row '${code}' to switch off; that is left out.`);
      continue;
    }
    for (let i = 1; i <= 7; i++) {
      row['func' + i] = '';
    }
  }
});

// itemstatcost.txt (checked above)
edit(statsTable, (table) => {
  while (table.rows.length > 0 && freeRow(table.rows[table.rows.length - 1])) {
    table.rows.pop();
  }
  for (const e of DATA.stats.filter((e) => !e.made)) {
    put(table, table.rows[e.id], e.cells);
  }
  for (const e of madeStats) {
    if (e.id === table.rows.length) {
      const row: TSVDataRow = {};
      table.headers.forEach((h) => (row[h] = ''));
      if ('*ID' in row) {
        row['*ID'] = String(e.id);
      }
      if ('*eol' in row) {
        row['*eol'] = '0';
      }
      table.rows.push(row);
    }
    put(table, table.rows[e.id], e.cells);
  }
  // disabled (Live Tables): no effect, still saved - op, its links, description and item events empty
  for (const name of DATA.statsOff) {
    const row = table.rows.find((r) => r.Stat === name);
    if (!row) {
      console.warn(`Live Export: itemstatcost.txt has no stat '${name}' to disable; that is left out.`);
      continue;
    }
    put(table, row, Object.fromEntries(DATA.statOffColumns.map((c) => [c, ''])));
  }
});

// The affix tables: by row (the number a recipe's pre= / suf= gives), checked by name.
const newNames: { key: string; text: string }[] = [];
for (const [path, list, off] of [
  ['global\\excel\\magicsuffix.txt', DATA.suffixes, DATA.suffixesOff],
  ['global\\excel\\magicprefix.txt', DATA.prefixes, DATA.prefixesOff],
  ['global\\excel\\automagic.txt', DATA.automagic, DATA.automagicOff],
] as [string, typeof DATA.prefixes, typeof DATA.prefixesOff][]) {
  if (list.length === 0 && off.length === 0) {
    continue;
  }
  edit(path, (table) => {
    for (const e of list) {
      const row = table.rows[e.row];
      if (!row || (row.Name ?? '') !== e.was) {
        console.warn(`Live Export: ${path} row ${e.row} is not '${e.was}' here; that change is left out.`);
        continue;
      }
      Object.assign(row, e.cells);
      if (e.newName) {
        row.Name = e.newName.key;
        newNames.push(e.newName);
      }
    }
    // disabled (Live Properties): rolls on no drop, no recipe can force it
    for (const e of off) {
      const row = table.rows[e.row];
      if (!row || (!e.edited && e.was !== '' && (row.Name ?? '') !== e.was)) {
        console.warn(`Live Export: ${path} row ${e.row} is not '${e.was}' here; disabling it is left out.`);
        continue;
      }
      row.spawnable = '0';
      row.frequency = '0';
    }
  });
}
if (newNames.length > 0) {
  const path = 'local\\lng\\strings\\item-nameaffixes.json';
  const strings = D2RMM.readJson(path) as { [key: string]: string | number }[];
  for (const n of newNames) {
    const entry: { [key: string]: string | number } = { id: D2RMM.getNextStringID(), Key: n.key };
    for (const language of ['enUS', 'zhTW', 'deDE', 'esES', 'frFR', 'itIT', 'koKR', 'plPL', 'esMX', 'jaJP', 'ptBR', 'ruRU', 'zhCN']) {
      entry[language] = n.text;
    }
    strings.push(entry);
  }
  D2RMM.writeJson(path, strings);
}

// cubemain.txt: the recipes taken out of the list, the ones put in.
for (const [path, cube] of [
  ['global\\excel\\cubemain.txt', DATA.cube],
  ['global\\excel\\base\\cubemain.txt', DATA.cubeBase],
] as [string, typeof DATA.cube][]) {
  if (cube.added.length === 0 && cube.removed.length === 0) {
    continue;
  }
  edit(path, (table) => {
    const same = (row: TSVDataRow, want: { [key: string]: string }) => cube.columns.every((c) => (row[c] ?? '') === (want[c] ?? ''));
    for (const gone of cube.removed) {
      const at = table.rows.findIndex((r) => same(r, gone));
      if (at >= 0) {
        table.rows.splice(at, 1);
      } else {
        console.warn(`Live Export: ${path} has no recipe '${gone.description ?? ''}' to take out.`);
      }
    }
    for (const row of cube.added) {
      const full: TSVDataRow = {};
      table.headers.forEach((h) => (full[h] = row[h] ?? ''));
      table.rows.push(full);
    }
  });
}
)TS";

std::string D2rmmTs(const Edits& edits, const Tsv& baseCube, const Tsv& baseCubeBase, const std::function<bool(const std::string&)>& isKey, Report& report) {
	std::string out = "// Made by Live Export (Live Suite): the changes made in the game with Live Properties and Live\n"
	                  "// Cube Recipes, as a D2RMM mod. Install it after the mods it was made with.\n\n"
	                  "const DATA = {\n";
	// properties
	out += "  properties: [";
	static const char* const kinds[] = { "func", "stat", "set", "val" };
	for (size_t e = 0; e < edits.props.size(); ++e) {
		std::vector<std::pair<std::string, std::string>> cells;
		for (int line = 0; line < 7; ++line) {
			for (int k = 0; k < 4; ++k) {
				const std::string& cell = edits.props[e].cells[static_cast<size_t>(line * 4 + k)];
				cells.emplace_back(kinds[k] + std::to_string(line + 1), k == 1 ? cell : Plain(cell));
			}
		}
		out += (e == 0 ? "\n    " : ",\n    ") + std::string("{ code: ") + Json(edits.props[e].code) + ", cells: " + Object(cells) + " }";
		report.done.push_back("properties: " + edits.props[e].code);
	}
	out += edits.props.empty() ? "],\n" : "\n  ],\n";
	out += "  madeProperties: [";
	for (size_t e = 0; e < edits.madeProps.size(); ++e) {
		const auto grown = edits.madePropCells.find(edits.madeProps[e]);
		out += (e == 0 ? "" : ", ") + std::string("{ code: ") + Json(edits.madeProps[e]) + ", cells: " + Object(grown != edits.madePropCells.end() ? grown->second : StatEdit::Cells {}) + " }";
		report.done.push_back("properties: " + edits.madeProps[e] + (grown != edits.madePropCells.end() ? " (made, grown in Live Tables)" : " (made)"));
	}
	out += "],\n";
	out += "  propertiesOff: [";
	for (size_t e = 0; e < edits.propsOff.size(); ++e) {
		out += (e == 0 ? "" : ", ") + Json(edits.propsOff[e]);
		report.done.push_back("properties: " + edits.propsOff[e] + " switched off");
	}
	out += "],\n";
	// stats
	out += "  stats: [";
	bool first = true;
	for (const StatEdit& s : edits.stats) {
		const bool                                       made = s.made;
		std::vector<std::pair<std::string, std::string>> cells;
		for (const auto& [column, value] : s.grownCells) {
			if (column != "*ID") {
				cells.emplace_back(column, value);
			}
		}
		for (int i = 0; i < kStatCells && s.lined; ++i) {
			const std::string& cell = s.cells[static_cast<size_t>(i)];
			if (i == kStrPosCell || i == kStrNegCell) {
				if (cell.empty()) {
					cells.emplace_back(kStatColumns[i], "");
				} else if (const auto key = edits.stringKeys.find(atol(cell.c_str())); key != edits.stringKeys.end()) {
					cells.emplace_back(kStatColumns[i], key->second);
				} else if (!IsNumber(cell)) {
					cells.emplace_back(kStatColumns[i], cell);
				} else if (edits.noString.count(atol(cell.c_str())) == 0) {
					report.problems.push_back("itemstatcost: stat " + std::to_string(s.id) + " " + kStatColumns[i] + ": string number " + cell + " has no known key; left as the table has it");
				}
			} else {
				cells.emplace_back(kStatColumns[i], i >= 12 ? cell : Plain(cell));
			}
		}
		if (made && !s.name.empty()) {
			cells.emplace_back("Stat", s.name);
		}
		out += (first ? "\n    " : ",\n    ") + std::string("{ id: ") + std::to_string(s.id) + ", was: " + Json(s.was) + ", made: " + (made ? "true" : "false") + ", cells: " + Object(cells) + " }";
		first = false;
		report.done.push_back("itemstatcost: stat " + std::to_string(s.id) + " " + (made ? s.name + (s.grownCells.empty() ? " (made)" : " (made, grown in Live Tables)") : s.was));
	}
	out += edits.stats.empty() ? "],\n" : "\n  ],\n";
	out += "  statsOff: [";
	for (size_t e = 0; e < edits.statsOff.size(); ++e) {
		out += (e == 0 ? "" : ", ") + Json(edits.statsOff[e].name);
		report.done.push_back("itemstatcost: " + edits.statsOff[e].name + " disabled");
	}
	out += "],\n  statOffColumns: [";
	for (size_t c = 0; c < 10; ++c) {
		out += (c == 0 ? "" : ", ") + Json(kStatOffColumns[c]);
	}
	out += "],\n";
	// affixes
	static const char* const names[] = { "suffixes", "prefixes", "automagic" };
	static const char* const files[] = { "magicsuffix", "magicprefix", "automagic" };
	for (int kind = 0; kind < 3; ++kind) {
		out += std::string("  ") + names[kind] + ": [";
		first = true;
		for (const AffixEdit& a : edits.affixes) {
			if (a.kind != kind) {
				continue;
			}
			std::vector<std::pair<std::string, std::string>> cells;
			for (int i = 1; i < kAffixCells; ++i) {
				if (i == 8 || i == 9 || i == 10) {
					continue; // class cells: numbers in the game, codes in the table
				}
				const std::string& cell = a.cells[static_cast<size_t>(i)];
				cells.emplace_back(kAffixColumns[i], cell.size() > 1 && cell[0] == '#' ? std::string() : Plain(cell));
			}
			const std::string& words = a.cells[0];
			std::string        named = "null";
			if (words.empty() || isKey(words)) {
				cells.emplace_back("Name", words);
			} else {
				named = "{ key: " + Json(std::string("live_") + files[kind] + "_" + std::to_string(a.index)) + ", text: " + Json(words) + " }";
			}
			out += (first ? "\n    " : ",\n    ") + std::string("{ row: ") + std::to_string(a.index) + ", was: " + Json(a.was) + ", cells: " + Object(cells) + ", newName: " + named + " }";
			first = false;
			report.done.push_back(std::string(files[kind]) + " " + std::to_string(a.index) + ": " + words);
		}
		out += first ? "],\n" : "\n  ],\n";
		out += std::string("  ") + names[kind] + "Off: [";
		first = true;
		for (const AffixOff& off : edits.affixesOff) {
			if (off.kind != kind) {
				continue;
			}
			bool edited = false;
			for (const AffixEdit& a : edits.affixes) {
				edited = edited || (a.kind == kind && a.index == off.index);
			}
			out += (first ? "" : ", ") + std::string("{ row: ") + std::to_string(off.index) + ", was: " + Json(off.was) + ", edited: " + (edited ? "true" : "false") + " }";
			first = false;
			report.done.push_back(std::string(files[kind]) + " " + std::to_string(off.index) + " " + off.was + " disabled (spawnable 0, frequency 0)");
		}
		out += "],\n";
	}
	// cube
	for (int b = 0; b < 2; ++b) {
		const Tsv&                            list = b == 0 ? edits.recipes : edits.recipesBase;
		const Tsv&                            base = b == 0 ? baseCube : baseCubeBase;
		std::vector<std::string>              columns;
		std::vector<std::vector<std::string>> added, removed;
		if (!list.rows.empty() && !base.headers.empty()) {
			CubeDiff(base, list, columns, added, removed);
			// Exalted Items' rows are never in D2RMM's tables (it adds them at game start): a changed
			// one goes in whole and replaces Exalted Items' own; a removed one goes in with enabled 0.
			size_t at[4] = { columns.size(), columns.size(), columns.size(), columns.size() }; // description, enabled, output, input 1
			for (size_t c = 0; c < columns.size(); ++c) {
				const char* names[] = { "description", "enabled", "output", "input 1" };
				for (int n = 0; n < 4; ++n) {
					at[n] = columns[c] == names[n] ? c : at[n];
				}
			}
			const auto cell = [&](const std::vector<std::string>& row, int n) { return at[n] < row.size() ? row[at[n]] : std::string(); };
			std::set<std::string> keys;
			size_t                changed = 0, gone = 0;
			for (const auto& row : added) {
				if (IsExalted(cell(row, 0))) {
					keys.insert(ExaltedKey(cell(row, 2), cell(row, 3)));
					++changed;
				}
			}
			std::vector<std::vector<std::string>> kept;
			for (auto row : removed) {
				if (!IsExalted(cell(row, 0))) {
					kept.push_back(row);
				} else if (keys.insert(ExaltedKey(cell(row, 2), cell(row, 3))).second && at[1] < row.size()) {
					row[at[1]] = "0";
					added.push_back(row);
					++gone;
				}
			}
			removed = kept;
			report.done.push_back(std::string(b == 0 ? "cubemain" : "base/cubemain") + ": " + std::to_string(added.size() - changed - gone) + " recipes in, " + std::to_string(removed.size()) + " out");
			if (changed + gone != 0) {
				report.done.push_back(std::string(b == 0 ? "cubemain" : "base/cubemain") + ": " + std::to_string(changed) + " Exalted Items recipes changed, " + std::to_string(gone)
				                      + " removed (written with enabled 0); the exalted codes in them are this install's");
			}
		}
		std::string cols = "[";
		for (size_t c = 0; c < columns.size(); ++c) {
			cols += (c == 0 ? "" : ", ") + Json(columns[c]);
		}
		out += std::string("  ") + (b == 0 ? "cube" : "cubeBase") + ": {\n    columns: " + cols + "],\n    added: " + Rows(columns, added) + ",\n    removed: " + Rows(columns, removed) + "\n  },\n";
	}
	out += "};\n";
	return out + kTsRuntime;
}

} // namespace exporter
