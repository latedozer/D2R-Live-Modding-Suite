// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// Live Tables native test.
//   grow_test hash <image dump> [tables...]   the game's own hash and lower-case routines, run from
//                                             the dump, against the port
//   grow_test grow <itemstatcost.txt> <properties.txt> [label]
//                                             a copied table grown in the model == the same tables
//                                             loaded with the rows already in the text, byte for byte
#include "../table_model.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

using namespace lt;

static int g_fail = 0, g_pass = 0;
static void Expect(bool ok, const std::string& what) {
	if (ok) ++g_pass; else { ++g_fail; printf("FAIL  %s\n", what.c_str()); }
}
static std::string ReadFile(const char* p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream s; s << f.rdbuf(); return s.str();
}

// ---------------------------------------------------------------- hash ------------
static void HashTest(const char* dumpPath, const std::vector<std::string>& names) {
	const int fd = open(dumpPath, O_RDONLY);
	if (fd < 0) { printf("SKIP  no image dump at %s\n", dumpPath); return; }
	const off_t size = lseek(fd, 0, SEEK_END);
	// file offset == RVA in the dump, so the code finds its tables (rip-relative) where it expects them
	uint8_t* image = static_cast<uint8_t*>(mmap(nullptr, size_t(size), PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0));
	close(fd);
	if (image == MAP_FAILED) { Expect(false, "mmap of the dump"); return; }
	static const uint8_t kHashStart[] = {0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20}; // 0x122c750
	static const uint8_t kLowerStart[] = {0x80, 0x39, 0x00, 0x4c, 0x8d, 0x04, 0x11};             // 0xa25650
	if (memcmp(image + 0x122c750, kHashStart, sizeof kHashStart) || memcmp(image + 0xa25650, kLowerStart, sizeof kLowerStart)) {
		Expect(false, "the dump has other bytes at 0x122c750 / 0xa25650");
		return;
	}
	using HashFn  = void(__attribute__((ms_abi)) *)(uint64_t* out, const char* data, size_t len, uint32_t seed);
	using LowerFn = void(__attribute__((ms_abi)) *)(char* text, size_t len);
	const auto gameHash  = reinterpret_cast<HashFn>(image + 0x122c750);
	const auto gameLower = reinterpret_cast<LowerFn>(image + 0xa25650);
	size_t n = 0, bad = 0;
	std::vector<std::string> all = names;
	for (int len = 0; len <= 40; ++len) all.push_back(std::string(size_t(len), char('a' + len % 26))); // every tail length
	all.push_back("Life_Steal_Per_Level"); all.push_back("\xc4\xd6\xdc mixed CASE 123");
	for (const std::string& s : all) {
		std::string g = s;
		gameLower(g.data(), g.size());
		uint64_t a[2], b[2];
		gameHash(a, g.data(), g.size(), 0);
		Murmur3(g.data(), g.size(), 0, b);
		++n;
		if (g != Lower(s) || a[0] != b[0] || a[1] != b[1])
			if (++bad <= 5) printf("FAIL  hash of '%s'\n", s.c_str());
	}
	Expect(bad == 0, "the game's own hash + lower-case == the port");
	printf("hash  %zu names hashed by the game's code from the dump and by the port: %zu differ\n", n, bad);
	munmap(image, size_t(size));
}

// ---------------------------------------------------------------- grow ------------
static std::string RowFor(const std::vector<std::string>& header, const std::map<std::string, std::string>& cells) {
	std::string r;
	for (size_t i = 0; i < header.size(); ++i) {
		if (i) r += '\t';
		auto it = cells.find(Lower(header[i]));
		if (it != cells.end()) r += it->second;
	}
	return r;
}
static std::string WithRow(std::string text, const std::string& row) {
	if (!text.empty() && text.back() != '\n') text += "\r\n";
	return text + row + "\r\n";
}

static bool SameNames(const NameTable& a, const NameTable& b, std::string* first) {
	for (const auto& n : a.owned) if (b.Find(n->key) != a.Find(n->key)) { *first = n->key; return false; }
	for (const auto& n : b.owned) if (a.Find(n->key) != b.Find(n->key)) { *first = n->key; return false; }
	return a.next == b.next && a.count == b.count;
}

static void Same(const Model& ref, const Model& live, const std::string& what) {
	Expect(ref.stats.rows == live.stats.rows && ref.stats.bytes == live.stats.bytes, what + ": every ItemStatCost row byte for byte (op links too)");
	Expect(ref.props.rows == live.props.rows && ref.props.bytes == live.props.bytes, what + ": every Properties row byte for byte");
	Expect(ref.descOrder == live.descOrder, what + ": the description order (+0x1278)");
	std::string first;
	Expect(SameNames(ref.names[StatNames], live.names[StatNames], &first), what + ": stat names give the same numbers " + first);
	Expect(SameNames(ref.names[PropNames], live.names[PropNames], &first), what + ": property codes give the same numbers " + first);
	if (ref.stats.bytes != live.stats.bytes)
		for (size_t i = 0; i < ref.stats.bytes.size() && i < live.stats.bytes.size(); ++i)
			if (ref.stats.bytes[i] != live.stats.bytes[i]) { printf("      first difference: stat %zu +0x%zx\n", i / kStatRow, i % kStatRow); break; }
	if (ref.props.bytes != live.props.bytes)
		for (size_t i = 0; i < ref.props.bytes.size() && i < live.props.bytes.size(); ++i)
			if (ref.props.bytes[i] != live.props.bytes[i]) { printf("      first difference: property %zu +0x%zx\n", i / kPropRow, i % kPropRow); break; }
}

static void GrowTest(const char* iscPath, const char* propPath, const std::string& L) {
	const std::string isc = ReadFile(iscPath), prop = ReadFile(propPath);
	if (isc.empty() || prop.empty()) { printf("SKIP  %s: tables not readable\n", L.c_str()); return; }
	const Sheet is = ReadSheet(isc), ps = ReadSheet(prop);
	Model base; Load(base, isc, prop);
	const size_t stats0 = base.stats.rows, props0 = base.props.rows;
	printf("%s  %zu stats (name table: next %d, %zu buckets), %zu properties (next %d, %zu buckets)\n", L.c_str(),
	       stats0, base.names[0].next, base.names[0].buckets.size(), props0, base.names[1].next, base.names[1].buckets.size());
	Expect(size_t(base.names[0].next) == stats0 && size_t(base.names[1].next) == props0, L + ": name numbers == row numbers at load");

	// 1. a stat in the RegenCharm shape + a property that names it
	const std::string statRow = RowFor(is.header, {
		{"stat", "lt_made_stat"}, {"signed", "1"}, {"send bits", "13"}, {"saved", "1"}, {"save bits", "13"},
		{"op", "2"}, {"op param", "12"}, {"op base", "maxhp"}, {"op stat1", "hpregen"},
		{"descpriority", "60"}, {"descfunc", "4"}, {"descval", "1"}, {"descstrpos", "ModStr1u"}, {"descstrneg", "ModStr1u"}});
	const std::string propRow = RowFor(ps.header, {{"code", "lt_made_prop"}, {"func1", "1"}, {"stat1", "lt_made_stat"}});
	Model ref; Load(ref, WithRow(isc, statRow), WithRow(prop, propRow));
	Model live; Load(live, isc, prop);
	const std::vector<uint8_t> before = live.stats.bytes;
	const uint8_t* oldBegin = live.stats.bytes.data(); // what a reader that took begin earlier holds
	const GrowResult gs = Grow(live, TableId::ItemStatCost, statRow);
	Expect(gs.ok && gs.number == stats0, L + ": stat grown as number " + std::to_string(stats0) + " " + gs.why);
	const GrowResult gp = Grow(live, TableId::Properties, propRow);
	Expect(gp.ok && gp.number == props0, L + ": property grown as number " + std::to_string(props0) + " " + gp.why);
	Same(ref, live, L + " stat + property");
	Expect(live.stats.retired.size() == 1 && live.stats.retired[0].data() == oldBegin && live.stats.retired[0] == before,
	       L + ": the old rows are still where an earlier reader has them, unchanged");
	printf("%s  grown: stat %zu (bucket rebuild: %s), property %zu (%s)\n", L.c_str(), gs.number, gs.rehash ? "YES" : "no", gp.number, gp.rehash ? "YES" : "no");

	// 2. the row as column=value cells == the row under the header
	{
		Model a; Load(a, isc, prop);
		std::vector<std::string> cells; std::string why;
		RowCells(is.header, statRow, cells, why);
		Expect(Grow(a, TableId::ItemStatCost, PairsOf(is.header, cells)).ok && a.stats.bytes == [&] { Model b; Load(b, isc, prop); Grow(b, TableId::ItemStatCost, statRow); return b.stats.bytes; }(),
		       L + ": column=value cells give the same row");
	}

	// 3. another plugin's in-place edit of an old row is carried into the grown table
	{
		Model a; Load(a, isc, prop);
		a.props.bytes[5 * kPropRow + 0x18] = 0; // e.g. Live Properties' Disable: func1 = 0
		Grow(a, TableId::ItemStatCost, statRow);
		Grow(a, TableId::Properties, propRow);
		Expect(a.props.bytes[5 * kPropRow + 0x18] == 0, L + ": an edit made in place before the growth is still there");
	}

	// refusals
	{
		Model t; Load(t, isc, prop);
		Expect(!Grow(t, TableId::ItemStatCost, RowFor(is.header, {{"stat", "maxhp"}})).ok, L + ": a taken stat name is refused");
		Expect(!Grow(t, TableId::Properties, RowFor(ps.header, {{"code", "ac"}})).ok, L + ": a taken property code is refused");
		Expect(!Grow(t, TableId::ItemStatCost, RowFor(is.header, {{"op", "1"}})).ok, L + ": a row without a name is refused");
		Expect(!Grow(t, TableId::ItemStatCost, std::string(60, 'x')).ok, L + ": a name the game would cut is refused");
		t.stats.sheet.rows.pop_back();
		Expect(!Grow(t, TableId::ItemStatCost, statRow).ok, L + ": a table that no longer matches its text is refused");
		Expect(t.stats.rows == stats0 && t.props.rows == props0, L + ": refusals change nothing");
	}

	// 4. many stats one after the other, up to the retail limit: each grown table == the loaded one
	{
		Model t; Load(t, isc, prop);
		std::string text = isc;
		size_t made = 0, rebuilds = 0, mismatches = 0;
		for (size_t k = 0;; ++k) {
			const std::string row = RowFor(is.header, {{"stat", "lt_s" + std::to_string(k)}, {"saved", "1"}, {"save bits", "8"},
			                                           {"descfunc", k % 3 ? "1" : ""}, {"descpriority", std::to_string(int(k % 50) - 10)},
			                                           {"op", k % 4 == 0 ? "1" : ""}, {"op base", k % 4 == 0 ? "strength" : ""},
			                                           {"op stat1", k % 4 == 0 ? (k ? "lt_s" + std::to_string(k - 1) : "dexterity") : ""}});
			const bool rehash = t.names[0].InsertRehashes();
			const GrowResult g = Grow(t, TableId::ItemStatCost, row);
			if (!g.ok) {
				Expect(t.stats.rows == kMaxStatsRetail, L + ": growth stops at 511 stats " + g.why);
				printf("%s  %zu stats made until the retail limit refused one: \"%s\"\n", L.c_str(), made, g.why.c_str());
				break;
			}
			++made; rebuilds += rehash;
			text = WithRow(text, row);
			if (k % 16 == 0 || t.stats.rows == kMaxStatsRetail) {
				Model r2; Load(r2, text, prop);
				if (r2.stats.bytes != t.stats.bytes || r2.descOrder != t.descOrder) ++mismatches;
			}
		}
		Expect(mismatches == 0, L + ": after every 16th growth the grown table == the table loaded with those rows");
		printf("%s  name-bucket rebuilds on the way: %zu (each one happens at the safe point)\n", L.c_str(), rebuilds);
		Model wide; wide.retailSaves = false; Load(wide, isc, prop);
		Expect(Grow(wide, TableId::ItemStatCost, RowFor(is.header, {{"stat", "lt_wide"}})).ok, L + ": with the wide save format the same row is taken");
	}

	// 5. OLDER rows that name the new name are re-linked: grown == loaded, byte for byte.
	//    Old stat rows whose op base / op stat names it (op links in load order), an old property's stat1.
	{
		const std::string early = RowFor(ps.header, {{"code", "lt_early"}, {"func1", "1"}, {"stat1", "lt_future"}, {"func2", "1"}, {"stat2", "strength"}});
		const std::string s1 = RowFor(is.header, {{"stat", "lt_old1"}, {"op", "2"}, {"op param", "1"}, {"op base", "strength"}, {"op stat1", "lt_future"}, {"op stat2", "hpregen"}});
		const std::string s2 = RowFor(is.header, {{"stat", "lt_old2"}, {"op", "1"}, {"op base", "lt_future"}, {"op stat1", "hpregen"}});
		const std::string s3 = RowFor(is.header, {{"stat", "lt_old3"}, {"op", "2"}, {"op base", "strength"}, {"op stat1", "hpregen"}, {"maxstat", "lt_future"}});
		const std::string future = RowFor(is.header, {{"stat", "lt_future"}, {"op", "1"}, {"op base", "strength"}, {"op stat1", "hpregen"}});
		std::string olds = WithRow(WithRow(WithRow(isc, s1), s2), s3);
		Model r2; Load(r2, WithRow(olds, future), WithRow(prop, early));
		Model l2; Load(l2, olds, WithRow(prop, early));
		const GrowResult g = Grow(l2, TableId::ItemStatCost, future);
		Expect(g.ok && g.relinked.size() == 3 && g.relinkedProps.size() == 1, L + ": 3 older stat rows and 1 older property re-linked");
		Same(r2, l2, L + " re-linked older rows");
	}

	// 7. remove a middle row and the last row: == the text loaded without that row, byte for byte
	{
		const std::string a = RowFor(is.header, {{"stat", "lt_a"}, {"saved", "1"}, {"save bits", "8"}, {"op", "1"}, {"op base", "strength"}, {"op stat1", "dexterity"}, {"descfunc", "1"}, {"descpriority", "5"}});
		const std::string b = RowFor(is.header, {{"stat", "lt_b"}, {"saved", "1"}, {"save bits", "9"}, {"op", "2"}, {"op param", "3"}, {"op base", "lt_a"}, {"op stat1", "hpregen"}, {"descfunc", "2"}, {"descpriority", "7"}});
		const std::string c = RowFor(is.header, {{"stat", "lt_c"}, {"saved", "1"}, {"save bits", "10"}, {"op", "1"}, {"op base", "lt_a"}, {"op stat1", "lt_a"}, {"maxstat", "lt_a"}, {"descfunc", "3"}, {"descpriority", "6"}});
		const std::string pc = RowFor(ps.header, {{"code", "lt_pc"}, {"func1", "1"}, {"stat1", "lt_c"}, {"func2", "1"}, {"stat2", "lt_a"}});
		Model t; Load(t, isc, prop);
		Expect(Grow(t, TableId::ItemStatCost, a).ok && Grow(t, TableId::ItemStatCost, b).ok && Grow(t, TableId::ItemStatCost, c).ok && Grow(t, TableId::Properties, pc).ok, L + ": 3 stats + a property grown");
		Expect(!RemoveStat(t, stats0).ok, L + ": a row other rows name (lt_a: op base of lt_b, lt_c; the property's stat2) is refused");
		const GrowResult rb = RemoveStat(t, stats0 + 1);                    // lt_b: in the middle, nothing names it
		Expect(rb.ok, L + ": the middle row lt_b removed " + rb.why);
		Model ref; Load(ref, WithRow(WithRow(isc, a), c), WithRow(prop, pc));
		Same(ref, t, L + " middle row removed (lt_c and the property's stat1 one lower)");
		Expect(!RemoveStat(t, stats0 + 1).ok, L + ": lt_c is named by the property: refused");
		Model t2; Load(t2, isc, prop);
		Grow(t2, TableId::ItemStatCost, a); Grow(t2, TableId::ItemStatCost, b);
		Expect(RemoveStat(t2, stats0 + 1).ok, L + ": the last row removed");
		Model ref2; Load(ref2, WithRow(isc, a), prop);
		Same(ref2, t2, L + " last row removed");
		Expect(Grow(t2, TableId::ItemStatCost, b).ok && t2.stats.rows == stats0 + 2, L + ": the freed number is grown again");
	}

	// 8. disable / enable: the row goes inert in place, and comes back exactly
	{
		const std::string a = RowFor(is.header, {{"stat", "lt_off"}, {"saved", "1"}, {"save bits", "8"}, {"op", "2"}, {"op param", "4"}, {"op base", "strength"}, {"op stat1", "dexterity"}, {"op stat2", "hpregen"}, {"descfunc", "1"}, {"descpriority", "5"}, {"dgrp", "3"}});
		Model t; Load(t, isc, prop);
		Grow(t, TableId::ItemStatCost, a);
		const std::vector<uint8_t> before = t.stats.bytes;
		const std::vector<uint16_t> order = t.descOrder;
		uint8_t* rows = t.stats.bytes.data();
		const OffCells was = DisableStat(rows, t.stats.rows, stats0);
		t.descOrder = DescOrder(rows, t.stats.rows);
		bool linked = false;
		for (size_t r = 0; r < t.stats.rows; ++r) {
			const uint8_t* x = rows + r * kStatRow;
			for (int e = 0; e < 16; ++e) { uint16_t s; memcpy(&s, x + 0xda + 6 * e + 2, 2); if (x[0xda + 6 * e + 4] != 0 && s == stats0) linked = true; }
			for (int k = 0; k < 64; ++k) { uint16_t s; memcpy(&s, x + 0x5a + 2 * k, 2); if (s == stats0) linked = true; }
		}
		Expect(IsInert(rows + stats0 * kStatRow) && !linked, L + ": disabled: no op, no text, no event, in no op list");
		Expect(std::find(t.descOrder.begin(), t.descOrder.end(), uint16_t(stats0)) == t.descOrder.end(), L + ": disabled: not in the description order");
		Expect(memcmp(rows + stats0 * kStatRow + 0x14, before.data() + stats0 * kStatRow + 0x14, 0x10) == 0, L + ": disabled: the save cells are as they were");
		EnableStat(rows, t.stats.rows, stats0, was);
		t.descOrder = DescOrder(rows, t.stats.rows);
		Expect(t.stats.bytes == before && t.descOrder == order, L + ": enabled again: every byte and the order as before");
	}

	// 9. rename a grown stat and a grown property: names, the rows that name it, and the next start
	{
		const std::string a = RowFor(is.header, {{"stat", "lt_ra"}, {"saved", "1"}, {"save bits", "8"}});
		const std::string b = RowFor(is.header, {{"stat", "lt_rb"}, {"op", "1"}, {"op base", "lt_ra"}, {"op stat1", "hpregen"}});
		const std::string pr = RowFor(ps.header, {{"code", "lt_rp"}, {"func1", "1"}, {"stat1", "lt_ra"}});
		Model t; Load(t, isc, prop);
		Grow(t, TableId::ItemStatCost, a); Grow(t, TableId::ItemStatCost, b); Grow(t, TableId::Properties, pr);
		const std::vector<uint8_t> statsBefore = t.stats.bytes, propsBefore = t.props.bytes;
		Expect(!RenameRow(t, TableId::ItemStatCost, stats0, "maxhp").ok, L + ": a rename to a taken name is refused");
		Expect(RenameRow(t, TableId::ItemStatCost, stats0, "lt_ra_new").ok && RenameRow(t, TableId::Properties, props0, "lt_rp_new").ok, L + ": stat and property renamed");
		Expect(t.names[StatNames].Find("lt_ra_new") == int32_t(stats0) && t.names[StatNames].Find("lt_ra") < 0 && t.names[PropNames].Find("lt_rp_new") == int32_t(props0),
		       L + ": the new names give the same numbers; the old one is gone");
		Expect(t.stats.bytes == statsBefore && t.props.bytes == propsBefore, L + ": a rename changes no row byte");
		// kept rows renamed == the texts loaded with the new names
		std::vector<KeptRow> kept;
		for (const auto& [tab, row] : std::vector<std::pair<TableId, std::string>>{{TableId::ItemStatCost, a}, {TableId::ItemStatCost, b}, {TableId::Properties, pr}}) {
			const Sheet& sh = tab == TableId::ItemStatCost ? is : ps;
			std::vector<std::string> cells; std::string why;
			RowCells(sh.header, row, cells, why);
			KeptRow k; k.table = tab; k.versions = "3"; k.name = NameIn(sh.header, cells, tab);
			for (size_t i = 0; i < sh.header.size(); ++i) if (!cells[i].empty()) k.cells.push_back({sh.header[i], cells[i]});
			kept.push_back(k);
		}
		const size_t c1 = RenameInKept(kept, TableId::ItemStatCost, "lt_ra", "lt_ra_new");
		const size_t c2 = RenameInKept(kept, TableId::Properties, "lt_rp", "lt_rp_new");
		Expect(c1 == 3 && c2 == 1, L + ": kept cells renamed (lt_ra: its name, lt_rb's op base, lt_rp's stat1; lt_rp: its code)");
		std::vector<std::string> added, notes;
		const std::string isc2 = AppendKept(isc, TableId::ItemStatCost, {&kept[0], &kept[1]}, added, notes);
		const std::string prop2 = AppendKept(prop, TableId::Properties, {&kept[2]}, added, notes);
		Model next; Load(next, isc2, prop2);
		Same(next, t, L + " renamed rows at the next start");
	}

	// 6. kept rows: written as lines, read back, laid into the text at the next load == the grown tables
	{
		Model t; Load(t, isc, prop);
		std::vector<KeptRow> kept;
		for (const auto& [tab, row] : std::vector<std::pair<TableId, std::string>>{{TableId::ItemStatCost, statRow}, {TableId::Properties, propRow}}) {
			const GrowResult g = Grow(t, tab, row);
			KeptRow k; k.table = tab; k.versions = "3"; k.number = g.number;
			const Sheet& sh = t.Of(tab).sheet;
			for (size_t i = 0; i < sh.header.size(); ++i) if (!sh.rows.back()[i].empty()) k.cells.push_back({sh.header[i], sh.rows.back()[i]});
			kept.push_back(k);
		}
		std::string file = "# Live Tables\n";
		for (const KeptRow& k : kept) file += KeptLine(k) + "\n";
		std::vector<std::string> problems;
		const std::vector<KeptRow> back = ReadKept(file, &problems);
		Expect(back.size() == 2 && problems.empty() && back[0].name == "lt_made_stat" && back[1].name == "lt_made_prop" && back[0].number == stats0, L + ": kept rows read back");
		std::vector<std::string> added, notes;
		const std::string isc2 = AppendKept(isc, TableId::ItemStatCost, {&back[0]}, added, notes);
		const std::string prop2 = AppendKept(prop, TableId::Properties, {&back[1]}, added, notes);
		Model next; Load(next, isc2, prop2);
		Expect(added.size() == 2 && notes.empty(), L + ": both rows laid into the texts");
		Same(next, t, L + " next start with the kept rows");
		added.clear();
		notes.clear();
		std::vector<Placed> placed;
		AppendKept(isc2, TableId::ItemStatCost, {&back[0]}, added, notes, &placed);
		Expect(added.empty() && notes.size() == 1 && placed.size() == 1 && placed[0].already && placed[0].number == stats0,
		       L + ": an exported mod already holds the kept row at its number: not added again, found at " + std::to_string(stats0));
		Expect(MovedRows(placed, TableId::ItemStatCost, 0).empty(), L + ": ... and that is no moved number (no alarm)");
		// the mod itself got two rows more below the made one (a mod update): the made row is appended 2 lower... higher
		std::string grownMod = WithRow(WithRow(isc, RowFor(is.header, {{"stat", "mod_new_1"}})), RowFor(is.header, {{"stat", "mod_new_2"}}));
		placed.clear(); added.clear(); notes.clear();
		AppendKept(grownMod, TableId::ItemStatCost, {&back[0]}, added, notes, &placed);
		const std::vector<Moved> mv = MovedRows(placed, TableId::ItemStatCost, 0);
		Expect(added.size() == 1 && mv.size() == 1 && mv[0].was == stats0 && mv[0].now == stats0 + 2, L + ": a mod update below the made row: moved " + std::to_string(stats0) + " -> " + std::to_string(stats0 + 2));
		// an exported mod that holds it at ANOTHER number
		std::string exportedElsewhere = WithRow(WithRow(isc, RowFor(is.header, {{"stat", "mod_new_1"}})), statRow);
		placed.clear(); added.clear(); notes.clear();
		AppendKept(exportedElsewhere, TableId::ItemStatCost, {&back[0]}, added, notes, &placed);
		const std::vector<Moved> mv2 = MovedRows(placed, TableId::ItemStatCost, 0);
		Expect(added.empty() && mv2.size() == 1 && mv2[0].now == stats0 + 1, L + ": the mod's table holds it at another number: moved alarm");
	}
}

static void KeptNumbers() { // per data version (a property is 299 in excel\base\, 303 in excel\)
	const std::string file = "itemstatcost\tv=123\tn=303\tnb=299\tstat=lt_x\n"
	                         "properties\tv=12\tn=299\tcode=lt_b\n"
	                         "properties\tv=3\tn=303\tcode=lt_c\n"
	                         "properties\tv=123\tn=299\tcode=lt_old\n"; // a 0.3.0 line: one number for both texts
	std::vector<std::string> problems;
	const std::vector<KeptRow> k = ReadKept(file, &problems);
	Expect(k.size() == 4 && problems.empty(), "kept lines with nb= read");
	Expect(KeptNumberFor(k[0], 0) == 303 && KeptNumberFor(k[0], 1) == 299, "grown in both: 303 in excel, 299 in base");
	Expect(KeptNumberFor(k[1], 1) == 299 && KeptNumberFor(k[1], 0) == SIZE_MAX, "grown in 1/2 only: base 299, excel none");
	Expect(KeptNumberFor(k[2], 0) == 303 && KeptNumberFor(k[2], 1) == SIZE_MAX, "grown in 3 only");
	Expect(KeptNumberFor(k[3], 0) == 299 && KeptNumberFor(k[3], 1) == SIZE_MAX, "an old line: base unknown (no false warning)");
	Expect(KeptLine(k[0]) == "itemstatcost\tv=123\tn=303\tnb=299\tstat=lt_x" && KeptLine(k[1]).find("nb=") == std::string::npos, "nb= written only when known");
}

static void JournalSteps() { // the remove stopped after each step: what the next start does
	KeptRow a; a.table = TableId::ItemStatCost; a.name = "lt_mid"; a.versions = "3"; a.number = 390;
	KeptRow b; b.table = TableId::ItemStatCost; b.name = "lt_after"; b.versions = "3"; b.number = 391;
	const std::vector<KeptRow> before = {a, b};   // grown-rows.txt before the remove
	const std::vector<KeptRow> after = {b};       // ... after it (written before the table changes)
	Expect(DecideJournal("", before).what == JournalStep::None, "no journal: nothing to do");
	// 1. journal written, Save Scan's fix not done (or stopped): no backup yet
	JournalStep s = DecideJournal(JournalLine("lt_mid", 390, ""), before);
	Expect(s.what == JournalStep::StoppedInFix && s.name == "lt_mid" && s.number == 390, "stopped in the fix: Save Scan's own restore");
	// 2. fix done, journal names the backup, grown-rows.txt not yet written: the saves go back
	s = DecideJournal(JournalLine("lt_mid", 390, "D:\\backup-20261125-230000"), before);
	Expect(s.what == JournalStep::Restore && s.backup == "D:\\backup-20261125-230000", "fixed, files not written: restore(backup)");
	// 3. grown-rows.txt written (the table changes next and cannot fail), journal not yet deleted
	s = DecideJournal(JournalLine("lt_mid", 390, "D:\\backup-20261125-230000"), after);
	Expect(s.what == JournalStep::Ended, "files written: the remove counts as done (the next start reads the table without it)");
	Expect(DecideJournal(JournalLine("LT_MID", 390, "x"), before).what == JournalStep::Restore, "names compared without case");
	// a cell change with a save resize (save bits 13 -> 6)
	KeptRow c = a; c.cells = {{"Stat", "lt_mid"}, {"Save Bits", "13"}};
	const std::string j = CellsJournalLine("lt_mid", 390, "B", {{"Save Bits", "6"}});
	Expect(DecideJournal(CellsJournalLine("lt_mid", 390, "", {{"Save Bits", "6"}}), {c}).what == JournalStep::StoppedInFix, "cells: stopped in the resize fix");
	Expect(DecideJournal(j, {c}).what == JournalStep::Restore, "cells: resized saves, grown-rows.txt still 13: restore");
	KeptRow c2 = c; SetKeptCells(c2, {{"save bits", "6"}});
	Expect(KeptCell(c2, "SAVE BITS") == "6" && c2.cells.size() == 2, "cells set case-insensitively in place");
	Expect(DecideJournal(j, {c2}).what == JournalStep::Ended, "cells: grown-rows.txt holds 6: ended");
	SetKeptCells(c2, {{"Save Bits", ""}, {"descfunc", "4"}});
	Expect(KeptCell(c2, "save bits").empty() && KeptCell(c2, "descfunc") == "4", "an empty value takes a cell out, a new one is added");
}

static void Reconcile() { // the user's own files of 2026-11-25: stat-edits says 6 save bits, grown-rows 13
	std::vector<std::string> problems;
	std::vector<KeptRow> kept = ReadKept(
		"itemstatcost\tv=123\tn=389\tStat=stat477\tSigned=1\tSend Bits=13\tSave Bits=13\n"
		"itemstatcost\tv=123\tn=390\tStat=stat478\tSigned=1\tSend Bits=13\tSave Bits=13\n"
		"itemstatcost\tv=123\tn=391\tStat=stat391\tSigned=1\tSend Bits=13\tSave Bits=13\n"
		"properties\tv=123\tn=299\tcode=%dmg\n", &problems);
	const std::string edits =
		"# Live Properties: the stat rows that were changed or made in the Stats tab.\n"
		"389\t1\t6\t6\t-\t-\t66\t-\t-\t5382\t5382\t2\t3\tlevel\tvitality\t-\t-\tstat477\tstat477\n"
		"390\t1\t13\t13\t-\t-\t-\t-\t-\t5382\t5382\t-\t-\t-\t-\t-\t-\tstat478\tstat478\r\n"
		"391\t-\t13\t13\t-\t-\t-\t-\t-\t5382\t5382\t-\t-\t-\t-\t-\t-\tstat391\tstat391\n"
		"12\t-\t9\t9\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\tlevel\n";
	std::vector<std::string> notes;
	const bool changed = ReconcileSaveShape(kept, edits, notes);
	Expect(changed && notes.size() == 2, "two cells differ (stat477's send bits and save bits)");
	Expect(KeptCell(kept[0], "Save Bits") == "6" && KeptCell(kept[0], "Send Bits") == "6", "stat477: send bits and save bits 13 -> 6 (the client reads with send bits)");
	Expect(KeptCell(kept[1], "Save Bits") == "13" && KeptCell(kept[2], "Signed") == "1", "same values stay; '-' = not set there, the kept one stays");
	Expect(KeptLine(kept[0]).find("Save Bits=6") != std::string::npos, "written back as Save Bits=6");
	notes.clear();
	Expect(!ReconcileSaveShape(kept, edits, notes) && notes.empty(), "a second start: nothing to change");
}

static void PerVersion() { // disable(table, 0, n): targets {1, 3}, the row found by name in each
	std::vector<VersionRow> rows;
	Expect(PairVersions({1, 3}, {389, 391}, rows) && rows.size() == 2, "{1, 3} paired with their own numbers");
	Expect(rows[0].version == 1 && rows[0].number == 389 && rows[1].version == 3 && rows[1].number == 391, "dv1 gets 389, dv3 gets 391 (not dv1's number)");
	Expect(!PairVersions({1, 3}, {389}, rows) && rows.empty(), "lists that do not pair: no rows");
	int32_t numbers[4] = {-1, -1, -1, -1};
	PairVersions({1, 3}, {389, 391}, rows);
	for (const VersionRow& r : rows) ReportNumber(numbers, r.version, r.number, true);
	Expect(numbers[0] == -1 && numbers[1] == 389 && numbers[2] == 389 && numbers[3] == 391, "done() numbers: [1] 389, [2] 389 (one object), [3] 391");
	int32_t two[4] = {-1, -1, -1, -1};
	ReportNumber(two, 1, 389, false); ReportNumber(two, 3, 391, false); ReportNumber(two, 7, 5, false); ReportNumber(two, 0, 5, false);
	Expect(two[0] == -1 && two[1] == 389 && two[2] == -1 && two[3] == 391, "1 and 2 apart: [2] stays -1; versions outside 1..3 write nothing");
	Expect(SameNumber(rows) == SIZE_MAX, "389 / 391: no one number for a save resize");
	PairVersions({1, 3}, {390, 390}, rows);
	Expect(SameNumber(rows) == 390, "390 / 390: one number");
}

int main(int argc, char** argv) {
	if (argc >= 3 && !strcmp(argv[1], "hash")) {
		std::vector<std::string> names;
		for (int i = 3; i < argc; ++i) {
			const Sheet s = ReadSheet(ReadFile(argv[i]));
			for (const auto& r : s.rows) names.push_back(r[0]);
		}
		HashTest(argv[2], names);
	} else if (argc >= 2 && !strcmp(argv[1], "kept")) {
		KeptNumbers();
		JournalSteps();
		Reconcile();
		PerVersion();
	} else if (argc >= 4 && !strcmp(argv[1], "grow")) {
		GrowTest(argv[2], argv[3], argc >= 5 ? argv[4] : "tables");
	} else {
		printf("usage: grow_test hash <dump> [tables...] | grow <itemstatcost.txt> <properties.txt> [label]\n");
		return 2;
	}
	printf("%s  %d checks passed, %d failed\n", g_fail ? "FAIL" : "ok  ", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
