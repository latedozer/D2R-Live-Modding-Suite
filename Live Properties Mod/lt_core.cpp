// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "lt_core.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace ltc {

std::string RowText(const std::vector<std::pair<std::string, std::string>>& cells) {
	std::string out;
	for (const auto& [column, value] : cells) {
		if (value.empty() || value == "-") {
			continue;
		}
		out += (out.empty() ? "" : "\t") + column + "=" + value;
	}
	return out;
}

std::string EditText(const std::vector<std::pair<std::string, std::string>>& cells) {
	std::string out;
	for (const auto& [column, value] : cells) {
		out += (out.empty() ? "" : "\t") + column + "=" + (value == "-" ? std::string() : value);
	}
	return out;
}

std::string NewStatRow(const std::string& name) {
	return RowText({ { "Stat", name }, { "Signed", "1" }, { "Send Bits", "13" }, { "Save Bits", "13" } });
}

std::string MadeStatRow(const std::string& name, const std::vector<std::string>& cells) {
	static const char* const columns[] = { "Signed", "Send Bits", "Save Bits", "Save Add", "ValShift" };
	std::vector<std::pair<std::string, std::string>> row { { "Stat", name } };
	for (size_t i = 0; i < 5 && i < cells.size(); ++i) {
		row.emplace_back(columns[i], cells[i]);
	}
	return RowText(row);
}

std::string NewPropertyRow(const std::string& code) {
	return RowText({ { "code", code } });
}

Handover PlanHandover(const std::vector<Line>& lines, const std::function<int(const std::string&)>& find, size_t count, size_t most) {
	Handover         plan;
	std::set<size_t> taken; // line numbers that stay
	std::vector<Line> sorted = lines;
	std::sort(sorted.begin(), sorted.end(), [](const Line& a, const Line& b) { return a.number < b.number; });
	for (const Line& line : sorted) {
		const int at = find(line.name);
		if (at >= 0 && static_cast<size_t>(at) != line.number) {
			plan.renumber[line.number] = static_cast<size_t>(at);
			plan.notes.push_back(line.name + ": the table has it as row " + std::to_string(at) + ", its line says " + std::to_string(line.number) + " - the line follows the table");
		} else if (at < 0) {
			plan.grow.push_back(line);
		}
	}
	size_t next = count;
	for (auto it = plan.grow.begin(); it != plan.grow.end();) {
		if (next > most) {
			plan.notes.push_back(it->name + ": no number left at or below " + std::to_string(most) + " - not handed over");
			it = plan.grow.erase(it);
			continue;
		}
		plan.expected[it->number] = next;
		plan.notes.push_back(it->name + ": " + std::to_string(it->number) + (it->was.empty() ? "" : " (on '" + it->was + "')") + " -> " + std::to_string(next) + ", grown by Live Tables");
		for (const size_t n : { it->number, next }) { // each number once (a line already at its new number: once)
			if (std::find(plan.scan.begin(), plan.scan.end(), static_cast<uint16_t>(n)) == plan.scan.end()) {
				plan.scan.push_back(static_cast<uint16_t>(n));
			}
		}
		++next;
		++it;
	}
	return plan;
}

bool GrowQueue::Start(const livetables::Api* api, std::vector<Item> items, Each each, End end) {
	if (api == nullptr || items.empty() || busy_.exchange(true)) {
		return false;
	}
	api_   = api;
	items_ = std::move(items);
	at_ = grown_ = 0;
	each_  = std::move(each);
	end_   = std::move(end);
	if (!Next()) {
		busy_.store(false);
		return false;
	}
	return true;
}

bool GrowQueue::Next() {
	const Item& item = items_[at_];
	return api_->grow(item.table, 0, item.rowText.c_str(), &GrowQueue::Done, this) != 0;
}

void __cdecl GrowQueue::Done(void* user, const livetables::GrowDone* done) noexcept {
	auto* q = static_cast<GrowQueue*>(user);
	const Item item = q->items_[q->at_];
	if (q->each_) {
		q->each_(item, *done);
	}
	std::string stopped;
	if (done->ok != 0) {
		++q->grown_;
	} else {
		stopped = done->why != nullptr ? done->why : "";
	}
	++q->at_;
	if (stopped.empty() && q->at_ < q->items_.size()) {
		if (q->Next()) {
			return;
		}
		stopped = "Live Tables did not take the next row";
	}
	const size_t grown = q->grown_, of = q->items_.size();
	End          end   = q->end_;
	q->busy_.store(false);
	if (end) {
		end(grown, of, stopped);
	}
}

std::vector<std::pair<std::string, int>> Listed(const std::vector<std::string>& names, const std::function<int(const std::string&)>& find) {
	std::vector<std::pair<std::string, int>> out;
	std::set<std::string>                    seen;
	for (const std::string& name : names) {
		if (name.empty() || !seen.insert(name).second) {
			continue;
		}
		const int at = find(name);
		if (at >= 0) {
			out.emplace_back(name, at);
		}
	}
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
	return out;
}

int IndexOf(const std::vector<std::string>& names, const std::string& name) {
	for (size_t i = 0; i < names.size(); ++i) {
		if (names[i] == name) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::string Used(int count, int limit) {
	return count < 0 ? std::string() : std::to_string(count) + " of " + std::to_string(limit) + " used";
}

void ChangeSlot::Record(const livetables::Change* change) noexcept {
	if (change == nullptr || change->dataVersion < 1 || change->dataVersion > 3 || (change->table != livetables::Table::ItemStatCost && change->table != livetables::Table::Properties)) {
		return;
	}
	const uint64_t head = head_.load(std::memory_order_relaxed);
	if (head - tail_.load(std::memory_order_acquire) >= kRoom) {
		lost_.store(true);
		return;
	}
	ltc::Record& r = ring_[head % kRoom];
	const bool   v2 = change->size >= offsetof(livetables::Change, mapSize) + sizeof(change->mapSize);
	r.kind        = change->kind;
	r.table       = change->table;
	r.dataVersion = change->dataVersion;
	r.oldRows     = change->oldRows;
	r.newRows     = change->newRows;
	r.oldCount    = change->oldCount;
	r.newCount    = change->newCount;
	r.number      = v2 ? change->number : 0;
	r.map         = v2 ? change->map : nullptr;
	r.mapSize     = v2 && change->map != nullptr ? change->mapSize : 0;
	const bool v3 = change->size >= offsetof(livetables::Change, newName) + sizeof(change->newName);
	auto       copy = [](char* to, const char* from) { // no allocation: a fixed copy, cut at 47 letters
		size_t n = 0;
		for (; from != nullptr && n + 1 < sizeof(ltc::Record::oldName) && from[n] != '\0'; ++n) {
			to[n] = from[n];
		}
		to[n] = '\0';
	};
	copy(r.oldName, v3 ? change->oldName : nullptr);
	copy(r.newName, v3 ? change->newName : nullptr);
	head_.store(head + 1, std::memory_order_release);
}

size_t ChangeSlot::Drain(ltc::Record* out, size_t room, bool& lost) noexcept {
	lost             = lost_.exchange(false);
	uint64_t       tail = tail_.load(std::memory_order_relaxed);
	const uint64_t head = head_.load(std::memory_order_acquire);
	size_t         n    = 0;
	for (; tail != head && n < room; ++tail, ++n) {
		out[n] = ring_[tail % kRoom];
	}
	tail_.store(tail, std::memory_order_release);
	return n;
}

bool Carry(std::vector<uint8_t>& orig, size_t rowSize, const ltc::Record& change, const uint8_t* now, uint64_t nowCount) {
	if (rowSize == 0 || orig.size() != change.oldCount * rowSize) {
		return false;
	}
	switch (change.kind) {
	case livetables::ChangeKind::Grow:
		if (now == nullptr || nowCount < change.oldCount) {
			return false;
		}
		orig.insert(orig.end(), now + change.oldCount * rowSize, now + nowCount * rowSize);
		return true;
	case livetables::ChangeKind::Shrink:
	case livetables::ChangeKind::Remove: {
		if (change.map == nullptr || change.mapSize != change.oldCount) {
			return false;
		}
		std::vector<uint8_t> next(change.newCount * rowSize, 0);
		for (uint64_t old = 0; old < change.oldCount; ++old) {
			const int32_t to = change.map[old];
			if (to >= 0 && static_cast<uint64_t>(to) < change.newCount) {
				memcpy(next.data() + static_cast<size_t>(to) * rowSize, orig.data() + old * rowSize, rowSize);
			}
		}
		orig.swap(next);
		return true;
	}
	default:
		return true; // Disable / Enable / Load: no row moves
	}
}

void RemapStatWords(std::vector<uint8_t>& rows, size_t rowSize, const std::vector<size_t>& offsets, const int32_t* map, uint64_t mapSize) {
	if (rowSize == 0 || map == nullptr) {
		return;
	}
	for (size_t at = 0; at + rowSize <= rows.size(); at += rowSize) {
		for (const size_t off : offsets) {
			if (off + 2 > rowSize) {
				continue;
			}
			uint16_t word;
			memcpy(&word, rows.data() + at + off, 2);
			if (word < mapSize) {
				const uint16_t to = map[word] < 0 ? 0xffff : static_cast<uint16_t>(map[word]);
				memcpy(rows.data() + at + off, &to, 2);
			}
		}
	}
}

bool RenameCell(std::string& cell, const std::string& from, const std::string& to) {
	if (from.empty() || cell != from) {
		return false;
	}
	cell = to;
	return true;
}

std::vector<uint16_t> RemoveScan(size_t number, size_t count) {
	std::vector<uint16_t> out;
	for (size_t n = number; n < count && n <= 0xffff; ++n) {
		out.push_back(static_cast<uint16_t>(n));
	}
	return out;
}

} // namespace ltc
