// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// lt_core against a fake Live Tables (the API of ../Live Tables/live_tables_api.h, in memory):
//   lt_test <stat-edits.txt>   (a copy of the user's file: made stats 477 / 478 on xlt_unused_ rows)
#include "../lt_core.h"
#include "../table_core.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

// ---- the fake: a stat table of 389 rows (the packed Exalted layout), a property table of 303
struct Fake {
	std::map<std::string, int> stats, props;
	int                        statCount = 389, propCount = 303;
	std::vector<std::string>   rows;      // every row text grow() got
	std::string                refuseNext; // grow's done says this once (e.g. "busy, try again")
	bool                       later = false;
	livetables::DoneFn         heldDone = nullptr;
	void*                      heldUser = nullptr;
	livetables::GrowDone       held {};
} g_fake;

static std::string Cell(const std::string& row, const std::string& column) {
	const std::string key = column + "=";
	size_t            at  = row.find(key);
	if (at == std::string::npos) {
		return {};
	}
	at += key.size();
	return row.substr(at, row.find('\t', at) == std::string::npos ? std::string::npos : row.find('\t', at) - at);
}

static int __cdecl FCheck(livetables::Table, int, const char*, char*, size_t) noexcept { return 1; }
static int __cdecl FGrow(livetables::Table table, int, const char* rowText, livetables::DoneFn done, void* user) noexcept {
	if (done == nullptr) {
		return 0;
	}
	g_fake.rows.push_back(rowText);
	livetables::GrowDone d {};
	d.size = sizeof(d);
	for (int& n : d.numbers) {
		n = -1;
	}
	if (!g_fake.refuseNext.empty()) {
		d.ok  = 0;
		d.why = g_fake.refuseNext.c_str();
	} else {
		const bool  stats = table == livetables::Table::ItemStatCost;
		const auto  name  = Cell(rowText, stats ? "Stat" : "code");
		int&        count = stats ? g_fake.statCount : g_fake.propCount;
		(stats ? g_fake.stats : g_fake.props)[name] = count;
		d.ok         = 1;
		d.numbers[1] = d.numbers[2] = count; // classic and LoD read the same text
		d.numbers[3] = count;
		++count;
		d.why = "";
	}
	if (g_fake.later) { // done comes later, on the UI thread
		g_fake.heldDone = done;
		g_fake.heldUser = user;
		g_fake.held     = d;
		return 1;
	}
	done(user, &d);
	return 1;
}
static int __cdecl FCount(livetables::Table table, int) noexcept { return table == livetables::Table::ItemStatCost ? g_fake.statCount : g_fake.propCount; }
static int __cdecl FLimit(livetables::Table table, int) noexcept { return table == livetables::Table::ItemStatCost ? 511 : 32767; }
static int __cdecl FFind(livetables::Table table, int, const char* name) noexcept {
	const auto& m  = table == livetables::Table::ItemStatCost ? g_fake.stats : g_fake.props;
	const auto  it = m.find(name);
	return it == m.end() ? -1 : it->second;
}
static uint64_t __cdecl FRevision() noexcept { return 1; }
static livetables::ChangeFn g_listen     = nullptr;
static void*                g_listenUser = nullptr;
static int __cdecl  FAdd(livetables::ChangeFn fn, void* user) noexcept {
	g_listen     = fn;
	g_listenUser = user;
	return 1;
}
// rename: the name table entry moves, the listener gets a Rename Change (names valid during the call only), then done()
static int __cdecl FRename(livetables::Table table, int, int number, const char* newName, livetables::DoneFn done, void* user) noexcept {
	auto&       m   = table == livetables::Table::ItemStatCost ? g_fake.stats : g_fake.props;
	std::string old;
	for (const auto& [name, n] : m) {
		old = n == number ? name : old;
	}
	livetables::GrowDone d {};
	d.size = sizeof(d);
	if (old.empty() || m.count(newName) != 0) {
		d.ok  = 0;
		d.why = "refused";
		done(user, &d);
		return 1;
	}
	m.erase(old);
	m[newName] = number;
	std::string        oldCopy = old, newCopy = newName; // gone after the call
	livetables::Change c { sizeof(livetables::Change), livetables::ChangeKind::Rename, table, 3, nullptr, nullptr, 0, 0, 0, 7, static_cast<uint64_t>(number), nullptr, 0, oldCopy.c_str(), newCopy.c_str() };
	if (g_listen != nullptr) {
		g_listen(g_listenUser, &c);
	}
	oldCopy.assign(oldCopy.size(), '?');
	d.ok = 1;
	d.numbers[3] = number;
	done(user, &d);
	return 1;
}
static void __cdecl FRemove(int) noexcept {}
static const livetables::Api kFake { sizeof(livetables::Api), livetables::kApiVersion, FCheck, FGrow, FCount, FLimit, FFind, FRevision, FAdd, FRemove, nullptr, nullptr, nullptr, nullptr, FRename };

static std::string Load(const char* path) {
	std::ifstream     in(path, std::ios::binary);
	std::stringstream all;
	all << in.rdbuf();
	return all.str();
}

// the made lines of a stat-edits.txt: number, cells 1-17, made name (17), row name (18)
static std::vector<std::pair<ltc::Line, std::vector<std::string>>> MadeLines(const std::string& file) {
	std::vector<std::pair<ltc::Line, std::vector<std::string>>> out;
	std::istringstream                                          in(file);
	std::string                                                 line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> cells;
		std::stringstream        cs(line);
		std::string              cell;
		while (std::getline(cs, cell, '\t')) {
			cells.push_back(cell);
		}
		if (cells.size() == 19 && cells[17] != "-") {
			out.push_back({ { static_cast<size_t>(atoi(cells[0].c_str())), cells[17], cells[18] }, std::vector<std::string>(cells.begin() + 1, cells.begin() + 17) });
		}
	}
	return out;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: lt_test <stat-edits.txt>\n");
		return 2;
	}
	const std::string edits = Load(argv[1]);
	const auto        made  = MadeLines(edits);
	Check(made.size() == 2 && made[0].first.number == 477 && made[0].first.name == "stat477" && made[1].first.was == "xlt_unused_478", "the user's stat-edits.txt: stat477 / stat478 on xlt_unused_477 / 478");

	// 1. row texts
	Check(ltc::NewStatRow("stat389") == "Stat=stat389\tSigned=1\tSend Bits=13\tSave Bits=13", "New stat's row: name, signed, 13 / 13 bits");
	Check(ltc::MadeStatRow("stat477", made[0].second) == "Stat=stat477\tSigned=1\tSend Bits=13\tSave Bits=13", "a made stat's row carries its saved shape from the line ('-' cells left out)");
	Check(ltc::NewPropertyRow("my-prop") == "code=my-prop", "New property's row: its code");

	// 2. the handover plan: neither name is in the table (389 rows) -> 389, 390
	auto find = [](const std::string& n) { return FFind(livetables::Table::ItemStatCost, 3, n.c_str()); };
	std::vector<ltc::Line> lines;
	for (const auto& [line, cells] : made) {
		lines.push_back(line);
	}
	const ltc::Handover h = ltc::PlanHandover(lines, find, 389, 510);
	Check(h.grow.size() == 2 && h.grow[0].name == "stat477" && h.grow[1].name == "stat478" && h.renumber.empty(), "handover: both made stats are grown, in number order");
	Check(h.expected.at(477) == 389 && h.expected.at(478) == 390, "handover: 477 -> 389, 478 -> 390 expected");
	Check(h.scan == std::vector<uint16_t>({ 477, 389, 478, 390 }), "Save Scan is asked for the old and the new numbers: 477 389 478 390");

	// 3. the queue grows them one after another through the fake; the numbers come from done()
	std::map<size_t, int> got;
	size_t                ended = 99, of = 0;
	std::string           stopped = "x";
	ltc::GrowQueue        queue;
	std::vector<ltc::GrowQueue::Item> items;
	for (size_t i = 0; i < made.size(); ++i) {
		items.push_back({ livetables::Table::ItemStatCost, ltc::MadeStatRow(made[i].first.name, made[i].second), made[i].first.name, made[i].first.number });
	}
	const bool started = queue.Start(&kFake, items, [&](const ltc::GrowQueue::Item& item, const livetables::GrowDone& d) { got[item.had] = d.ok ? d.numbers[3] : -1; },
		[&](size_t grown, size_t all, const std::string& why) { ended = grown; of = all; stopped = why; });
	Check(started && got[477] == 389 && got[478] == 390 && ended == 2 && of == 2 && stopped.empty() && !queue.Busy(), "grown: stat477 389, stat478 390; the queue is free again");
	Check(g_fake.rows.size() == 2 && Cell(g_fake.rows[1], "Stat") == "stat478", "the fake got both rows, stat478 second");

	// 4. stat-edits.txt follows the numbers done() gave (the plugin writes the moved lines; table_core does the text)
	std::map<size_t, size_t> moved;
	for (const auto& [from, to] : got) {
		moved[from] = static_cast<size_t>(to);
	}
	const std::string re = table::RenumberEdits(edits, moved);
	Check(re.find("\r\n389\t1\t13\t13\t") != std::string::npos && re.find("\tstat477\tstat477\r\n") != std::string::npos && re.find("\tstat478\tstat478\r\n") != std::string::npos && re.find("xlt_unused_") == std::string::npos,
	      "stat-edits.txt: lines 389 / 390, row names = the made names");
	Check(re.substr(0, re.find("\r\n389")) == edits.substr(0, edits.find("\r\n477")) && table::RenumberEdits(re, moved) == re && table::RenumberEdits(edits, {}) == edits,
	      "comment lines byte for byte the same; nothing moved = unchanged; a second run changes nothing");

	// 5. the next start: Live Tables laid the rows in; nothing is handed over again
	const ltc::Handover again = ltc::PlanHandover({ { 389, "stat477", "stat477" }, { 390, "stat478", "stat478" } }, find, 391, 510);
	Check(again.grow.empty() && again.renumber.empty() && again.scan.empty(), "after the handover: nothing to grow, nothing to renumber, no scan");
	// the file was not rewritten (e.g. it could not be written): the lines follow the table, no second growth
	const ltc::Handover late = ltc::PlanHandover(lines, find, 391, 510);
	Check(late.grow.empty() && late.renumber.at(477) == 389 && late.renumber.at(478) == 390, "lines still on 477 / 478 but the rows are there: renumbered to 389 / 390, nothing grown");

	// 6. a refusal: the queue stops, nothing after it is tried, the reason is passed on
	g_fake.refuseNext = "busy, try again";
	const size_t before = g_fake.rows.size();
	std::vector<ltc::GrowQueue::Item> two { { livetables::Table::ItemStatCost, ltc::NewStatRow("a"), "a" }, { livetables::Table::ItemStatCost, ltc::NewStatRow("b"), "b" } };
	queue.Start(&kFake, two, nullptr, [&](size_t grown, size_t, const std::string& why) { ended = grown; stopped = why; });
	Check(ended == 0 && stopped == "busy, try again" && g_fake.rows.size() == before + 1 && FFind(livetables::Table::ItemStatCost, 3, "b") < 0, "busy: stopped at the first row with Live Tables' words, the second not tried");
	g_fake.refuseNext.clear();

	// 7. done() later (as on the UI thread): the queue is busy until it comes
	g_fake.later = true;
	std::vector<ltc::GrowQueue::Item> one { { livetables::Table::Properties, ltc::NewPropertyRow("my-prop"), "my-prop" } };
	int propAt = -2;
	queue.Start(&kFake, one, [&](const ltc::GrowQueue::Item&, const livetables::GrowDone& d) { propAt = d.numbers[3]; }, nullptr);
	Check(queue.Busy() && propAt == -2 && !queue.Start(&kFake, one, nullptr, nullptr), "waiting for done(): busy, a second start is refused");
	g_fake.heldDone(g_fake.heldUser, &g_fake.held);
	Check(!queue.Busy() && propAt == 303, "done() came: my-prop is property 303, the queue is free");
	g_fake.later = false;
	Check(!queue.Start(nullptr, one, nullptr, nullptr) && !queue.Start(&kFake, {}, nullptr, nullptr), "no Live Tables, or nothing to grow: nothing queued");

	// 8. listMade from Live Tables' find, in row order; a made name without a row is left out
	const auto listed = ltc::Listed({ "stat478", "stat477", "gone", "stat477" }, find);
	Check(listed.size() == 2 && listed[0] == std::make_pair(std::string("stat477"), 389) && listed[1] == std::make_pair(std::string("stat478"), 390), "listMade: stat477 389, stat478 390; no row = not listed; no doubles");
	const auto lp = ltc::Listed({ "my-prop" }, [](const std::string& n) { return FFind(livetables::Table::Properties, 3, n.c_str()); });
	Check(lp.size() == 1 && lp[0].second == 303, "listMade: properties too");
	// every listed stat has its line, with the same name and number
	bool lined = true;
	for (const auto& [name, number] : listed) {
		lined = lined && re.find("\r\n" + std::to_string(number) + "\t") != std::string::npos && re.find("\t" + name + "\t" + name + "\r\n") != std::string::npos;
	}
	Check(lined, "every listed stat has its stat-edits.txt line (same name, same number)");

	// 8b. listMade as the plugin answers it: from the names walked from the game's name table (kept
	// current by the change slot), never from Live Tables (Live Export asks from its own thread)
	{
		std::vector<std::string> walked(389);
		for (size_t i = 0; i < walked.size(); ++i) {
			walked[i] = "game" + std::to_string(i);
		}
		walked.push_back("stat477"); // the change slot's grows, applied: rows 389, 390
		walked.push_back("stat478");
		int calls = 0;
		const auto byNames = ltc::Listed({ "stat478", "stat477", "gone" }, [&](const std::string& n) { ++calls; return ltc::IndexOf(walked, n); });
		const auto byLt    = ltc::Listed({ "stat478", "stat477", "gone" }, find);
		Check(byNames == byLt && byNames.size() == 2 && calls == 3 && ltc::IndexOf(walked, "game0") == 0 && ltc::IndexOf(walked, "nope") == -1,
		      "listMade from the walked names: the same answer as Live Tables' find, without calling it");
		std::vector<std::string> before(walked.begin(), walked.begin() + 389); // a listMade before the slot was applied
		Check(ltc::Listed({ "stat477" }, [&](const std::string& n) { return ltc::IndexOf(before, n); }).empty(), "before the grow is applied: not listed (only rows really in the table)");
	}

	// 9. N of M used
	Check(ltc::Used(FCount(livetables::Table::ItemStatCost, 3), FLimit(livetables::Table::ItemStatCost, 3)) == "391 of 511 used" && ltc::Used(-1, 511).empty(), "Stats: '391 of 511 used' (389 + the 2 grown; the refused one not counted)");

	// 10. the cap: a 510-row table takes one more, the second is not handed over
	const ltc::Handover cap = ltc::PlanHandover({ { 477, "x1", "" }, { 478, "x2", "" } }, [](const std::string&) { return -1; }, 510, 510);
	Check(cap.grow.size() == 1 && cap.expected.at(477) == 510 && !cap.notes.empty(), "cap: only one number left (510), the other is noted, not grown");

	// 11. the change queue: written without a lock, drained in order, Load kept as a record too
	ltc::ChangeSlot slot;
	uint8_t         a[4], b[4], c[4];
	livetables::Change g1 { sizeof(livetables::Change), livetables::ChangeKind::Grow, livetables::Table::ItemStatCost, 3, a, b, 389, 390, 0x144, 2, 389, nullptr, 0 };
	livetables::Change g2 { sizeof(livetables::Change), livetables::ChangeKind::Grow, livetables::Table::ItemStatCost, 3, b, c, 390, 391, 0x144, 3, 390, nullptr, 0 };
	slot.Record(&g1);
	slot.Record(&g2);
	ltc::Record rec[8];
	bool        lost = true;
	size_t      n    = slot.Drain(rec, 8, lost);
	Check(n == 2 && !lost && rec[0].oldRows == a && rec[0].newRows == b && rec[1].oldRows == b && rec[1].newRows == c && rec[1].number == 390, "two grows: both records, in order (a -> b, b -> c)");
	Check(slot.Drain(rec, 8, lost) == 0 && !lost && slot.Empty(), "drained: nothing left");
	for (size_t i = 0; i < ltc::ChangeSlot::kRoom + 3; ++i) {
		slot.Record(&g1);
	}
	n = slot.Drain(rec, 8, lost);
	Check(lost && n == 8, "more than the room before the plugin looked: lost() says so (the plugin takes the tables as they are)");
	while (slot.Drain(rec, 8, lost) != 0) {
	}
	livetables::Change v1 { offsetof(livetables::Change, number), livetables::ChangeKind::Grow, livetables::Table::Properties, 2, a, b, 299, 300, 0x30, 5, 77, nullptr, 9 };
	slot.Record(&v1);
	Check(slot.Drain(rec, 8, lost) == 1 && rec[0].number == 0 && rec[0].map == nullptr && rec[0].mapSize == 0, "a version 1 Change (no number / map fields): read as none");

	// 11b. Rename (Live Tables v3): the names are copied in the listener; an old v2 Change has none
	{
		ltc::ChangeSlot    rs;
		std::string        longName(60, 'x');
		livetables::Change rn { sizeof(livetables::Change), livetables::ChangeKind::Rename, livetables::Table::ItemStatCost, 3, a, a, 391, 391, 0x144, 9, 389, nullptr, 0, "stat477", "vit_per_level" };
		livetables::Change rl = rn;
		rl.newName            = longName.c_str();
		rs.Record(&rn);
		rs.Record(&rl);
		std::string gone = "stat477";
		gone[0]          = 'X'; // the Change's text changes after the call: the record keeps its copy
		const size_t got2 = rs.Drain(rec, 8, lost);
		Check(got2 == 2 && std::string(rec[0].oldName) == "stat477" && std::string(rec[0].newName) == "vit_per_level" && rec[0].number == 389, "Rename: old / new name and the number kept in the record");
		Check(std::string(rec[1].newName).size() == 47, "a name longer than the room is cut at 47 letters (Live Tables refuses those anyway)");
		livetables::Change r2 { offsetof(livetables::Change, oldName), livetables::ChangeKind::Grow, livetables::Table::ItemStatCost, 3, a, b, 389, 390, 0x144, 2, 389, nullptr, 0 };
		rs.Record(&r2);
		Check(rs.Drain(rec, 8, lost) == 1 && rec[0].oldName[0] == '\0' && rec[0].newName[0] == '\0', "a version 2 Change: no names read");
		std::map<std::string, int> edits { { "vit-str", 1 }, { "res-all", 2 } };
		Check(ltc::RenameKey(edits, "vit-str", "vit-per-str") == 1 && edits.count("vit-per-str") == 1 && edits.count("vit-str") == 0 && ltc::RenameKey(edits, "res-all", "vit-per-str") == 0,
		      "property-edits by code: the key follows the rename; a taken name is not overwritten");
		std::string cell = "stat477", other = "stat4770";
		Check(ltc::RenameCell(cell, "stat477", "vit_per_level") && cell == "vit_per_level" && !ltc::RenameCell(other, "stat477", "x") && other == "stat4770", "a cell naming the stat follows (exact names only)");
	}

	// 11c. a rename through the fake Live Tables: its listener call lands in the queue; the edit files follow by name
	{
		ltc::ChangeSlot q;
		kFake.addListener([](void* u, const livetables::Change* c) noexcept { static_cast<ltc::ChangeSlot*>(u)->Record(c); }, &q);
		bool renamed = false;
		kFake.rename(livetables::Table::Properties, 0, 303, "vit-per-str", [](void* u, const livetables::GrowDone* d) noexcept { *static_cast<bool*>(u) = d->ok != 0; }, &renamed);
		ltc::Record r[4];
		bool        l = false;
		const size_t got3 = q.Drain(r, 4, l);
		std::map<std::string, std::string> edits { { "my-prop", "func 1 stat477" } };
		const size_t moved = got3 == 1 ? ltc::RenameKey(edits, r[0].oldName, r[0].newName) : 0;
		Check(renamed && got3 == 1 && r[0].kind == livetables::ChangeKind::Rename && std::string(r[0].oldName) == "my-prop" && std::string(r[0].newName) == "vit-per-str" && moved == 1
		          && edits.count("vit-per-str") == 1 && FFind(livetables::Table::Properties, 3, "vit-per-str") == 303,
		      "fake rename my-prop -> vit-per-str: Change Rename queued with both names (copied), property-edits key follows, number 303 kept");
		bool again = true;
		kFake.rename(livetables::Table::Properties, 0, 303, "vit-per-str", [](void* u, const livetables::GrowDone* d) noexcept { *static_cast<bool*>(u) = d->ok != 0; }, &again);
		Check(!again && q.Drain(r, 4, l) == 0, "a taken name: refused, no Change");
		g_listen = nullptr;
	}

	// 12. the originals carried over by row number
	{
		const size_t         row = 4;
		std::vector<uint8_t> orig;
		for (uint8_t r = 0; r < 5; ++r) {
			orig.insert(orig.end(), row, r); // row r = r r r r
		}
		std::vector<uint8_t> now = orig;
		now.insert(now.end(), row, 9); // grown row 5 = 9 9 9 9
		std::vector<uint8_t> o = orig;
		Check(ltc::Carry(o, row, { livetables::ChangeKind::Grow, livetables::Table::ItemStatCost, 3, nullptr, now.data(), 5, 6, 5, nullptr, 0 }, now.data(), 6) && o.size() == 24 && o[20] == 9 && o[0] == 0,
		      "Grow: the 5 originals kept, the grown row appended as it is now");
		const int32_t mid[5] = { 0, 1, -1, 2, 3 }; // row 2 removed
		o                    = orig;
		Check(ltc::Carry(o, row, { livetables::ChangeKind::Remove, livetables::Table::ItemStatCost, 3, nullptr, nullptr, 5, 4, 2, mid, 5 }, nullptr, 4) && o.size() == 16 && o[8] == 3 && o[12] == 4 && o[4] == 1,
		      "Remove row 2: rows 3, 4 become 2, 3 (their originals move with them)");
		const int32_t last[5] = { 0, 1, 2, 3, -1 };
		o                     = orig;
		Check(ltc::Carry(o, row, { livetables::ChangeKind::Shrink, livetables::Table::ItemStatCost, 3, nullptr, nullptr, 5, 4, 4, last, 5 }, nullptr, 4) && o.size() == 16 && o[12] == 3,
		      "Shrink: the last row's original goes");
		o = orig;
		Check(!ltc::Carry(o, row, { livetables::ChangeKind::Remove, livetables::Table::ItemStatCost, 3, nullptr, nullptr, 6, 5, 2, mid, 5 }, nullptr, 5) && o == orig, "originals of another state (6 rows wanted, 5 kept): refused, untouched");
		std::map<uint16_t, std::string> lines { { 1, "one" }, { 2, "two" }, { 3, "three" }, { 4, "four" }, { 9, "past" } };
		std::vector<uint16_t>           dropped;
		const auto                      moved = ltc::RemapKeys(lines, mid, 5, dropped);
		Check(moved.size() == 4 && moved.at(1) == "one" && moved.at(2) == "three" && moved.at(3) == "four" && moved.at(9) == "past" && dropped == std::vector<uint16_t>({ 2 }),
		      "edit lines by number: 3 -> 2, 4 -> 3, the removed row's line dropped, a number past the map kept");
		std::vector<uint8_t> props(2 * 6, 0); // two rows of 6 bytes: a stat word at 0 and 4
		const uint16_t       w[] = { 3, 0xffff, 4, 1, 0, 2 };
		memcpy(props.data(), w, sizeof(w));
		ltc::RemapStatWords(props, 6, { 0, 4 }, mid, 5);
		uint16_t r2[6];
		memcpy(r2, props.data(), sizeof(r2));
		Check(r2[0] == 2 && r2[2] == 3 && r2[3] == 1 && r2[5] == 0xffff && r2[1] == 0xffff && r2[4] == 0, "stat words in saved rows: 3 -> 2, 4 -> 3, the removed 2 -> none; other cells untouched");
		Check(ltc::RemoveScan(389, 392) == std::vector<uint16_t>({ 389, 390, 391 }), "a middle-row Remove of 389 in a 392-row table: Save Scan asked for 389 390 391");
	}

	// 13. the 2026-11-25 21:01 run: lines already at 389 / 390 (renumbered earlier), no row yet -> each number scanned once
	{
		const ltc::Handover same = ltc::PlanHandover({ { 389, "stat477", "stat477" }, { 390, "stat478", "stat478" } }, [](const std::string&) { return -1; }, 389, 510);
		Check(same.grow.size() == 2 && same.expected.at(389) == 389 && same.expected.at(390) == 390 && same.scan == std::vector<uint16_t>({ 389, 390 }), "lines at their numbers already: grown at 389 / 390, Save Scan asked '389 390' (not '389 389 390 390')");
	}

	printf(g_failed == 0 ? "lt: all passed\n" : "lt: %d FAILED\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
