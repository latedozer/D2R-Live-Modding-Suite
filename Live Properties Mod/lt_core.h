// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties on Live Tables (../Live Tables/live_tables_api.h): the parts that do not touch the
// game, tested natively against a fake Live Tables (test/lt_test.cpp).
//  - the row texts New stat / New property hand to grow();
//  - the one-time handover of made stats that have no row in the game's table (477 / 478, made on
//    the older Exalted Items' filler rows) to Live Tables, which gives them the numbers after the
//    table's end and keeps them from then on;
//  - a queue that grows rows one after another (each done() starts the next);
//  - listMade's answer, "N of M used";
//  - the lock-free slot the change listener writes at Live Tables' safe point.
#pragma once

#include "../Live Tables/live_tables_api.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ltc {

// "column=value" cells, tab separated, in the order given; empty values are left out.
std::string RowText(const std::vector<std::pair<std::string, std::string>>& cells);
// setCells text: "column=value" cells, tab separated, in order; an empty value empties the cell.
std::string EditText(const std::vector<std::pair<std::string, std::string>>& cells);
// A new stat: its name, may be negative, 13 bits sent and saved (what Stats > New always made).
std::string NewStatRow(const std::string& name);
// A made stat's line handed over: its name and its saved shape (signed, send bits, save bits, save
// add, val shift = stat-edits.txt cells 0-4; "" or "-" = empty). The rest stays an edit of the line.
std::string MadeStatRow(const std::string& name, const std::vector<std::string>& cells);
// A new property: only its code (its lines are filled on its card).
std::string NewPropertyRow(const std::string& code);

// One made stat line of stat-edits.txt.
struct Line {
	size_t      number = 0;
	std::string name, was; // made name (column 17), the row's name when written (column 18)
};
struct Handover {
	std::vector<Line>        grow;     // no row of that name in the table: grow these, in number order
	std::map<size_t, size_t> renumber; // the table has the name at another number: line number -> row number
	std::map<size_t, size_t> expected; // line number -> the number the grow will give (count, count + 1 ...)
	std::vector<uint16_t>    scan;     // every old and new number, for Save Scan
	std::vector<std::string> notes;    // for the log
};
// find: the game's name table (Live Tables find) -> number or -1. count: the table's rows now.
// most: the highest number a stat may get (limit - 1).
Handover PlanHandover(const std::vector<Line>& lines, const std::function<int(const std::string&)>& find, size_t count, size_t most);

// Grows rows one after another through api->grow; `each` gets every result on the thread done()
// runs on; after the last one (or the first refusal: nothing after it is tried) `end` gets how many
// were grown. Start returns false when nothing was queued (no api, or grow refused at once).
class GrowQueue {
public:
	struct Item {
		livetables::Table table;
		std::string       rowText, name;
		size_t            had = 0; // the line's old number (handover) or 0
	};
	using Each = std::function<void(const Item&, const livetables::GrowDone&)>;
	using End  = std::function<void(size_t grown, size_t of, const std::string& stoppedWhy)>;
	bool Start(const livetables::Api* api, std::vector<Item> items, Each each, End end);
	bool Busy() const { return busy_.load(); }

private:
	static void __cdecl Done(void* user, const livetables::GrowDone* done) noexcept;
	bool                Next();
	const livetables::Api* api_ = nullptr;
	std::vector<Item>      items_;
	size_t                 at_ = 0, grown_ = 0;
	Each                   each_;
	End                    end_;
	std::atomic<bool>      busy_ { false };
};

// listMade: of `names` (made stat names / property codes), those the table has, with their numbers, in row order.
std::vector<std::pair<std::string, int>> Listed(const std::vector<std::string>& names, const std::function<int(const std::string&)>& find);

// The number of `name` in names walked from the game's name table (row number -> name), or -1.
// What listMade and the handover use: no Live Tables call, so any thread (under the plugin's lock).
int IndexOf(const std::vector<std::string>& names, const std::string& name);

// "389 of 511 used" (count < 0: "").
std::string Used(int count, int limit);

// The change listener's queue: written at Live Tables' point (atomics and a fixed array only: no
// lock, no allocation), drained at the plugin's next own entry, in order. Live Tables makes one
// change at a time, so there is one writer. Full (more than kRoom changes before the plugin
// looked): the record is dropped and lost() says so - the plugin then takes the tables as they are.
struct Record {
	livetables::ChangeKind kind;
	livetables::Table      table;
	int32_t                dataVersion;
	const uint8_t*         oldRows;
	const uint8_t*         newRows;
	uint64_t               oldCount, newCount;
	uint64_t               number;  // v2: the row a Disable / Enable / Shrink / Remove is about
	const int32_t*         map;     // Shrink / Remove: old -> new (-1 = removed), valid until the next table load
	uint64_t               mapSize;
	char                   oldName[48]; // v3 Rename: copied in the listener (the Change's are valid during the call only); "" = none
	char                   newName[48];
};
class ChangeSlot {
public:
	static constexpr size_t kRoom = 64;
	void   Record(const livetables::Change* change) noexcept;
	// Up to `room` records, oldest first; returns how many. lost: some were dropped since the last drain.
	size_t Drain(ltc::Record* out, size_t room, bool& lost) noexcept;
	bool   Empty() const noexcept { return head_.load() == tail_.load() && !lost_.load(); }

private:
	ltc::Record            ring_[kRoom] {};
	std::atomic<uint64_t> head_ { 0 }, tail_ { 0 }; // written: head (writer); read: tail (the plugin, under its lock)
	std::atomic<bool>     lost_ { false };
};

// The saved originals of a table (rows of rowSize bytes) after a change, the rows carried over by
// their number: Grow appends the new rows from `now`; Shrink / Remove move every row by the map
// (-1 = gone). Returns false when the originals do not have oldCount rows (they belong to another
// state of the table: the caller takes them anew).
bool Carry(std::vector<uint8_t>& orig, size_t rowSize, const ltc::Record& change, const uint8_t* now, uint64_t nowCount);

// Edit lines keyed by row number, after a Shrink / Remove: each key moved by the map, the removed
// row's line dropped (returned in `dropped`), keys past the map kept.
template <class V> std::map<uint16_t, V> RemapKeys(const std::map<uint16_t, V>& lines, const int32_t* map, uint64_t mapSize, std::vector<uint16_t>& dropped) {
	std::map<uint16_t, V> out;
	for (const auto& [key, value] : lines) {
		if (key >= mapSize) {
			out[key] = value;
		} else if (map[key] < 0) {
			dropped.push_back(key);
		} else {
			out[static_cast<uint16_t>(map[key])] = value;
		}
	}
	return out;
}

// Stat numbers kept inside saved rows (op base / op stats of stat rows, stat1-7 of property rows),
// after a middle-row Remove: each word below mapSize moved by the map (removed -> 0xffff, none).
void RemapStatWords(std::vector<uint8_t>& rows, size_t rowSize, const std::vector<size_t>& offsets, const int32_t* map, uint64_t mapSize);

// A rename: the keys of an edits map (by name / code) follow. Returns how many moved (0 or 1).
template <class V> size_t RenameKey(std::map<std::string, V>& lines, const std::string& from, const std::string& to) {
	const auto it = lines.find(from);
	if (from.empty() || to.empty() || from == to || it == lines.end() || lines.count(to) != 0) {
		return 0;
	}
	V value = it->second;
	lines.erase(it);
	lines[to] = value;
	return 1;
}
// A cell naming `from` gets `to` (exact match, as the cells keep names). True when changed.
bool RenameCell(std::string& cell, const std::string& from, const std::string& to);

// What a middle-row Remove of `number` touches in saves: the stat itself and every later one
// (renumbered), for Save Scan's scan before the question.
std::vector<uint16_t> RemoveScan(size_t number, size_t count);

} // namespace ltc
