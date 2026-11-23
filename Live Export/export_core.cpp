// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Export: edits into text tables, and the D2RMM mod (see export_core.h).
#include "export_core.h"

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
	return props.empty() && stats.empty() && affixes.empty() && recipes.rows.empty() && recipesBase.rows.empty();
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

std::vector<std::string> ReadStatEdits(const std::string& text, Edits& edits) {
	std::vector<std::string> bad;
	for (const auto& parts : Records(text)) {
		if (parts.size() != kStatCells + 3 || parts[0].empty()) {
			bad.push_back("stat-edits.txt: a line with " + std::to_string(parts.size()) + " cells was skipped");
			continue;
		}
		StatEdit edit;
		edit.id = atoi(parts[0].c_str());
		for (int i = 0; i < kStatCells; ++i) {
			edit.cells[static_cast<size_t>(i)] = Cell(parts[static_cast<size_t>(i + 1)]);
		}
		edit.name = Cell(parts[kStatCells + 1]);
		edit.was  = Cell(parts[kStatCells + 2]);
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

void ApplyProperties(Tsv& properties, const Edits& edits, Report& report) {
	const int code = properties.Column("code");
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
}

void ApplyStats(Tsv& isc, const Edits& edits, bool padStats, Report& report) {
	const int name = isc.Column("Stat"), id = isc.Column("*ID");
	if (name < 0) {
		report.problems.push_back("itemstatcost: the table has no Stat column; no stat edit is in the export");
		return;
	}
	// the stat number -> its name in this table (a stat cell may hold a number)
	const auto nameOf = [&](const std::string& cell) -> std::string {
		if (!IsNumber(cell)) {
			return cell;
		}
		const long n = atol(cell.c_str());
		return n >= 0 && static_cast<size_t>(n) < isc.rows.size() ? isc.rows[static_cast<size_t>(n)][static_cast<size_t>(name)] : cell;
	};
	for (const StatEdit& edit : edits.stats) {
		if (edit.id < 0) {
			continue;
		}
		const size_t row     = static_cast<size_t>(edit.id);
		const bool   made    = edit.was.empty() || edit.was.rfind("xlt_unused_", 0) == 0;
		if (row >= isc.rows.size()) {
			if (!made || !padStats) {
				report.problems.push_back("itemstatcost: stat " + std::to_string(edit.id) + " (" + edit.was + ") is past the table's end; that edit is not in the export");
				continue;
			}
			while (isc.rows.size() <= row) {
				std::vector<std::string> filler(isc.headers.size());
				filler[static_cast<size_t>(name)] = "live_unused_" + std::to_string(isc.rows.size());
				if (id >= 0) {
					filler[static_cast<size_t>(id)] = std::to_string(isc.rows.size());
				}
				isc.rows.push_back(filler);
			}
		}
		std::string& now = isc.At(row, "Stat");
		if (now != edit.was && !(made && (now.rfind("live_unused_", 0) == 0 || now.rfind("xlt_unused_", 0) == 0))) {
			report.problems.push_back("itemstatcost: stat " + std::to_string(edit.id) + " is '" + now + "' in this table, not '" + edit.was + "'; that edit is not in the export");
			continue;
		}
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
				} else {
					report.problems.push_back("itemstatcost: stat " + std::to_string(edit.id) + " " + kStatColumns[i] + ": string number " + cell + " has no known key; the table's own cell is kept");
				}
			} else if (i >= 12) {
				out = nameOf(cell);
			} else {
				out = Plain(cell);
			}
		}
		if (made && !edit.name.empty()) {
			now = edit.name;
		}
		report.done.push_back("itemstatcost: stat " + std::to_string(edit.id) + " " + (made ? (edit.name.empty() ? "(made)" : edit.name + " (made)") : edit.was));
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

void LeaveOutExaltedRows(Edits& edits, Report& report) {
	std::vector<StatEdit> stats;
	for (const StatEdit& s : edits.stats) {
		const bool filler = s.was.empty() || s.was.rfind("xlt_unused_", 0) == 0;
		if (s.was.rfind("xlt_", 0) == 0 && !filler) {
			report.problems.push_back("itemstatcost: stat " + std::to_string(s.id) + " " + s.was + ": not carried, Exalted Items makes this row");
		} else if (filler && s.id > 486) {
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

function edit(path: string, change: (table: TSVData) => void): void {
  const table = D2RMM.readTsv(path);
  if (!table) {
    return;
  }
  change(table);
  D2RMM.writeTsv(path, table);
}

// properties.txt: by code
edit('global\\excel\\properties.txt', (table) => {
  for (const e of DATA.properties) {
    const row = table.rows.find((r) => r.code === e.code);
    if (!row) {
      console.warn(`Live Export: properties.txt has no row '${e.code}'; that change is left out.`);
      continue;
    }
    Object.assign(row, e.cells);
  }
});

// itemstatcost.txt: by number (a stat's number is its row; saved items keep numbers). A stat
// made in the game sits at the number it had there: empty rows (live_unused_<n>) fill the table
// up to it. If another mod already put a stat at that number, this mod stops: an item holding
// the made stat would read as that other stat.
edit('global\\excel\\itemstatcost.txt', (table) => {
  for (const e of DATA.stats) {
    while (table.rows.length <= e.id) {
      const filler: TSVDataRow = {};
      table.headers.forEach((h) => (filler[h] = ''));
      filler.Stat = 'live_unused_' + table.rows.length;
      if ('*ID' in filler) {
        filler['*ID'] = String(table.rows.length);
      }
      table.rows.push(filler);
    }
    const row = table.rows[e.id];
    const free = row.Stat === e.was || (e.made && (row.Stat.startsWith('live_unused_') || row.Stat.startsWith('xlt_unused_')));
    if (!free) {
      throw new Error(`Live Export: stat ${e.id} is '${row.Stat}' here, not '${e.was}' - another mod uses that number. Saved items with it would read wrong, so nothing is changed.`);
    }
    Object.assign(row, e.cells);
  }
});

// The affix tables: by row (the number a recipe's pre= / suf= gives), checked by name.
const newNames: { key: string; text: string }[] = [];
for (const [path, list] of [
  ['global\\excel\\magicsuffix.txt', DATA.suffixes],
  ['global\\excel\\magicprefix.txt', DATA.prefixes],
  ['global\\excel\\automagic.txt', DATA.automagic],
] as [string, typeof DATA.prefixes][]) {
  if (list.length === 0) {
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
	// stats
	out += "  stats: [";
	bool first = true;
	for (const StatEdit& s : edits.stats) {
		const bool                                       made = s.was.empty() || s.was.rfind("xlt_unused_", 0) == 0;
		std::vector<std::pair<std::string, std::string>> cells;
		for (int i = 0; i < kStatCells; ++i) {
			const std::string& cell = s.cells[static_cast<size_t>(i)];
			if (i == kStrPosCell || i == kStrNegCell) {
				if (cell.empty()) {
					cells.emplace_back(kStatColumns[i], "");
				} else if (const auto key = edits.stringKeys.find(atol(cell.c_str())); key != edits.stringKeys.end()) {
					cells.emplace_back(kStatColumns[i], key->second);
				} else if (!IsNumber(cell)) {
					cells.emplace_back(kStatColumns[i], cell);
				} else {
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
		report.done.push_back("itemstatcost: stat " + std::to_string(s.id));
	}
	out += edits.stats.empty() ? "],\n" : "\n  ],\n";
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
