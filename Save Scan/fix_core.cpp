// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "fix_core.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace savescan {

namespace {

constexpr int kMaxId = 510; // 9-bit ids, 0x1ff ends a list

uint32_t Dword(const std::string& b, size_t at) {
	uint32_t v;
	memcpy(&v, b.data() + at, 4);
	return v;
}
void PutDword(std::string& b, size_t at, uint32_t v) {
	memcpy(b.data() + at, &v, 4);
}
unsigned Bit(const std::string& b, size_t bit) {
	return (static_cast<uint8_t>(b[bit >> 3]) >> (bit & 7)) & 1u;
}

struct BitOut {
	std::string bytes;
	size_t      at = 0;
	void put(uint64_t value, unsigned count) {
		for (unsigned i = 0; i < count; ++i, ++at) {
			if ((at >> 3) >= bytes.size()) {
				bytes.push_back('\0');
			}
			if (((value >> i) & 1u) != 0) {
				bytes[at >> 3] = static_cast<char>(static_cast<uint8_t>(bytes[at >> 3]) | (1u << (at & 7)));
			}
		}
	}
	void copy(const std::string& from, size_t begin, size_t end) {
		for (size_t bit = begin; bit < end; ++bit) {
			put(Bit(from, bit), 1);
		}
	}
};

FileResult ReadAs(bool stash, const std::string& bytes, const Tables& tables) {
	const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
	return stash ? ReadStash(data, bytes.size(), tables) : ReadCharacter(data, bytes.size(), tables);
}

std::string Name(const Item& item, size_t index) {
	return "item " + std::to_string(index + 1) + " (" + item.code + ", " + item.section + ", byte " + std::to_string(item.offset) + ")";
}

// The byte places in a file that hold a size, checksum or section length.
std::set<size_t> LengthFields(bool stash, const std::string& bytes) {
	std::set<size_t> at;
	if (!stash) {
		for (size_t i = 8; i < 16; ++i) {
			at.insert(i);
		}
		return at;
	}
	for (size_t p = 0; p + 20 <= bytes.size();) {
		const uint32_t length = Dword(bytes, p + 16);
		for (size_t i = 16; i < 20; ++i) {
			at.insert(p + i);
		}
		if (length < 64) {
			break;
		}
		p += length;
	}
	return at;
}

// The bytes outside the items, in order: [0, first item), [end of an item, next item) ..., [last end, file end).
std::vector<std::pair<size_t, size_t>> Gaps(const std::vector<Item>& items, size_t size) {
	std::vector<std::pair<size_t, size_t>> gaps;
	size_t                                 at = 0;
	for (const Item& item : items) {
		gaps.emplace_back(at, item.offset);
		at = (static_cast<size_t>(item.bitEnd) + 7) >> 3;
	}
	gaps.emplace_back(at, size);
	return gaps;
}

} // namespace

bool StashRecordTailSafe(const std::string& bytes, std::string& why, std::string& note) {
	for (size_t at = 0; at + 68 <= bytes.size();) {
		const uint32_t length = Dword(bytes, at + 16), type = Dword(bytes, at + 20);
		if (length < 64 || at + length > bytes.size()) {
			why = "a section's size is wrong (byte " + std::to_string(at) + ")";
			return false;
		}
		if (type == 2) {
			const size_t body = at + 64, end = at + length;
			if (body + 20 > end || Dword(bytes, body) != 0xc0eaedc0u) {
				why = "its record of found items (byte " + std::to_string(at) + ") does not start the known way, so what it holds is not known";
				return false;
			}
			const auto   word = [&](size_t o) { return static_cast<size_t>(static_cast<uint8_t>(bytes[o]) | static_cast<uint8_t>(bytes[o + 1]) << 8); };
			const size_t tail = body + 20 + (word(body + 6) + word(body + 8) + word(body + 10)) * 10;
			if (tail > end) {
				why = "its record of found items (byte " + std::to_string(at) + ") holds more entries than fit in it";
				return false;
			}
			const bool zero = bytes.find_first_not_of('\0', tail) >= end;
			note += (note.empty() ? "" : "; ") + std::to_string(end - tail) + " stale bytes after the record (the game counts its header twice)" + (zero ? ", all zero," : "") + " kept as is";
		}
		at += length;
	}
	return true;
}

uint64_t Fnv64(const std::string& bytes) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const char c : bytes) {
		hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
	}
	return hash;
}

FixOut Restore(const std::vector<RestoreEntry>& plan, FixIo& io) {
	FixOut      out;
	std::string why;
	if (!io.mayWrite(why)) {
		out.why = "Nothing was put back: " + why;
		return out;
	}
	std::string problems;
	for (const RestoreEntry& entry : plan) {
		FixFileOut report;
		report.name        = entry.name;
		report.bytesBefore = static_cast<int32_t>(entry.writtenSize);
		report.bytesAfter  = static_cast<int32_t>(entry.backup.size());
		std::string now;
		const bool  read = io.read(entry.name, now);
		if (read && now == entry.backup) {
			report.state = FixState::Unchanged;
			report.why   = "already as in the backup";
		} else if (read && now.size() == entry.writtenSize && Fnv64(now) == entry.writtenHash) {
			std::string back;
			if (!io.mayWrite(why)) {
				report.state = FixState::Refused;
				report.why   = "not put back: " + why;
				++out.notRestored;
			} else if (io.write(entry.name, entry.backup, why) && io.read(entry.name, back) && back == entry.backup) {
				report.state = FixState::Restored;
				++out.restored;
			} else {
				report.state = FixState::Refused;
				report.why   = "COULD NOT BE PUT BACK (" + (why.empty() ? std::string("it does not read back as the backup") : why) + ")";
				++out.notRestored;
			}
		} else {
			report.state = FixState::Refused;
			report.why   = read ? "it changed since the fix wrote it (the game saved it?), so it is left as it is" : "it could not be read";
			++out.notRestored;
		}
		if (report.state == FixState::Refused) {
			problems += (problems.empty() ? "" : "; ") + entry.name + ": " + report.why;
		}
		out.files.push_back(std::move(report));
	}
	out.ok  = out.notRestored == 0;
	out.why = out.ok ? (out.restored == 0 ? "Every file was already as in the backup." : std::to_string(out.restored) + " file(s) put back from the backup and read back.")
	                 : std::to_string(out.notRestored) + " file(s) NOT put back - " + problems + ".";
	return out;
}

std::vector<MadeRow> MadeStatRows(const std::string& grownRows) {
	std::vector<MadeRow> rows;
	size_t               from = 0;
	while (from < grownRows.size()) {
		size_t end = grownRows.find('\n', from);
		end        = end == std::string::npos ? grownRows.size() : end;
		std::string line = grownRows.substr(from, end - from);
		from             = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind("itemstatcost\t", 0) != 0) {
			continue;
		}
		MadeRow row;
		bool    numbered = false;
		for (size_t at = 0; at <= line.size();) {
			size_t tab = line.find('\t', at);
			tab        = tab == std::string::npos ? line.size() : tab;
			const std::string cell = line.substr(at, tab - at);
			at                     = tab + 1;
			const size_t eq        = cell.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			std::string key = cell.substr(0, eq);
			for (char& c : key) {
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			}
			const long value = strtol(cell.c_str() + eq + 1, nullptr, 10);
			if (key == "n") {
				row.number = static_cast<uint16_t>(value);
				numbered   = value >= 0 && value <= 0x7fff;
			} else if (key == "stat") {
				row.name = cell.substr(eq + 1);
			} else if (key == "save bits") {
				row.shape.saveBits = static_cast<uint8_t>(value);
			} else if (key == "save add") {
				row.shape.saveAdd = static_cast<int32_t>(value);
			} else if (key == "save param bits") {
				row.shape.paramBits = static_cast<uint8_t>(value);
			}
		}
		if (numbered) {
			rows.push_back(row);
		}
	}
	return rows;
}

std::vector<std::string> FilesHoldingMissing(const std::vector<FixInput>& files, const Tables& live, const std::vector<MadeRow>& rows, std::vector<MadeRow>& missing) {
	missing.clear();
	Tables wide = live;
	for (const MadeRow& row : rows) {
		if (row.number >= live.stats.size()) {
			missing.push_back(row);
			if (wide.stats.size() <= row.number) {
				wide.stats.resize(static_cast<size_t>(row.number) + 1);
			}
			wide.stats[row.number] = row.shape;
		}
	}
	std::vector<std::string> holding;
	if (missing.empty()) {
		return holding;
	}
	for (const FixInput& file : files) {
		const FileResult read = ReadAs(file.stash, file.bytes, wide);
		bool             holds = false;
		for (const Item& item : read.items) {
			for (const StatValue& value : item.stats) {
				holds = holds || value.stat >= live.stats.size();
			}
		}
		if (holds || (!read.ok && read.problem.find("past the stat table") != std::string::npos)) {
			holding.push_back(file.name);
		}
	}
	return holding;
}

bool InGroup(int id) {
	return id == 17 || id == 18 || (id >= 48 && id <= 59);
}

bool MakeMap(const Tables& before, const std::vector<ChangeSpec>& changes, const std::vector<ResizeSpec>& resizes, StatMap& map, std::string& problem, const std::vector<AffixSpec>& affixes) {
	map.affixTo[0].clear();
	map.affixTo[1].clear();
	for (const AffixSpec& change : affixes) {
		const char* kind = change.suffix != 0 ? "suffix" : "prefix";
		if (change.from < 1 || change.from > 2047 || change.to == 0 || change.to < -1 || change.to > 2047) {
			problem = std::string(kind) + " " + std::to_string(change.from) + " -> " + std::to_string(change.to) + ": saved affix ids are 1 to 2047 (0 = none), -1 drops one";
			return false;
		}
		if (!map.affixTo[change.suffix & 1].emplace(change.from, change.to).second) {
			problem = std::string(kind) + " " + std::to_string(change.from) + " is named twice in the affix change list";
			return false;
		}
	}
	for (int k = 0; k < 2; ++k) {
		std::set<int32_t> taken;
		for (const auto& [from, to] : map.affixTo[k]) {
			if (to < 0) {
				continue;
			}
			if (!taken.insert(to).second || (to != from && map.affixTo[k].count(static_cast<uint16_t>(to)) == 0)) {
				problem = std::string(k != 0 ? "suffix" : "prefix") + " " + std::to_string(from) + " would become " + std::to_string(to) + ", a number another affix keeps";
				return false;
			}
		}
	}
	const size_t count = before.stats.size();
	map.to.resize(count);
	map.shape   = before.stats;
	map.resized.assign(count, 0);
	for (size_t i = 0; i < count; ++i) {
		map.to[i] = static_cast<int32_t>(i);
	}
	std::set<uint16_t> named;
	for (const ChangeSpec& change : changes) {
		if (change.from >= count) {
			problem = "stat " + std::to_string(change.from) + " is past the stat table (" + std::to_string(count) + " stats)";
			return false;
		}
		if (!named.insert(change.from).second) {
			problem = "stat " + std::to_string(change.from) + " is named twice in the change list";
			return false;
		}
		if (change.to < -1 || change.to > kMaxId) {
			problem = "stat " + std::to_string(change.from) + " would get number " + std::to_string(change.to) + " (the saves hold 0 to 510)";
			return false;
		}
		if (change.to != change.from && (InGroup(change.from) || InGroup(change.to))) {
			problem = "stat " + std::to_string(change.from) + (change.to < 0 ? " would be dropped" : " would become " + std::to_string(change.to))
			          + ": stats 17-18, 48-55 and 57-59 are saved together in the game's own order, so they cannot move";
			return false;
		}
		map.to[change.from] = change.to;
	}
	std::set<uint16_t> resizedOnce;
	for (const ResizeSpec& resize : resizes) {
		if (resize.stat >= count || map.to[resize.stat] < 0) {
			problem = "a new saved shape for stat " + std::to_string(resize.stat) + (resize.stat >= count ? ", which is past the stat table" : ", which is dropped");
			return false;
		}
		if (!resizedOnce.insert(resize.stat).second) {
			problem = "stat " + std::to_string(resize.stat) + " is given two new saved shapes";
			return false;
		}
		if (resize.saveBits < 1 || resize.saveBits > 32 || resize.paramBits > 32) {
			problem = "stat " + std::to_string(resize.stat) + ": save bits must be 1 to 32 and save param bits 0 to 32";
			return false;
		}
		StatShape& shape = map.shape[resize.stat];
		if (shape.saveBits != resize.saveBits || shape.paramBits != resize.paramBits || shape.saveAdd != resize.saveAdd) {
			shape.saveBits  = resize.saveBits;
			shape.paramBits = resize.paramBits;
			shape.saveAdd   = resize.saveAdd;
			map.resized[resize.stat] = 1;
		}
	}
	size_t after = 0;
	for (size_t i = 0; i < count; ++i) {
		after = map.to[i] >= 0 && static_cast<size_t>(map.to[i]) + 1 > after ? static_cast<size_t>(map.to[i]) + 1 : after;
	}
	map.after.items = before.items;
	map.after.stats.assign(after, StatShape {});
	std::vector<int32_t> owner(after, -1);
	for (size_t i = 0; i < count; ++i) {
		const int32_t to = map.to[i];
		if (to < 0) {
			continue;
		}
		if (owner[static_cast<size_t>(to)] >= 0) {
			problem = "stats " + std::to_string(owner[static_cast<size_t>(to)]) + " and " + std::to_string(i) + " would both be number " + std::to_string(to);
			return false;
		}
		owner[static_cast<size_t>(to)] = static_cast<int32_t>(i);
		map.after.stats[static_cast<size_t>(to)] = map.shape[i];
		map.after.stats[static_cast<size_t>(to)].csvBits = before.stats[i].csvBits;
	}
	return true;
}

uint32_t CharacterChecksum(const std::string& bytes) {
	uint32_t sum = 0;
	for (size_t i = 0; i < bytes.size(); ++i) {
		const uint8_t byte = i >= 12 && i < 16 ? 0 : static_cast<uint8_t>(bytes[i]);
		sum                = ((sum << 1) | (sum >> 31)) + byte;
	}
	return sum;
}

bool RewriteFile(bool stash, const std::string& bytes, const FileResult& read, const StatMap& map, Rewrite& out, std::string& problem) {
	out = Rewrite {};
	if (!read.ok) {
		problem = "it cannot be read to its end: " + read.problem;
		return false;
	}
	std::string                            result;
	std::vector<std::pair<size_t, size_t>> ends; // an item's old end byte, the size change of all items up to it
	size_t                                 cursor = 0;
	int64_t                                delta  = 0;
	for (size_t n = 0; n < read.items.size(); ++n) {
		const Item& item = read.items[n];
		const size_t begin = item.offset, end = (static_cast<size_t>(item.bitEnd) + 7) >> 3;
		if (begin < cursor || end > bytes.size() || item.bitStart != begin * 8) {
			problem = Name(item, n) + ": its place in the file is not after the item before it";
			return false;
		}
		Item expect = item;
		expect.stats.clear();
		bool touched = false;
		for (const StatValue& value : item.stats) {
			touched = touched || map.touches(value.stat);
		}
		bool affixTouched = false;
		for (const AffixSlot& slot : item.affixes) {
			affixTouched = affixTouched || map.affix(slot.suffix, slot.id) != slot.id;
		}
		if (affixTouched && item.affixes.size() == 2 && !item.affixes[0].flagged) {
			const int32_t p = map.affix(0, item.affixes[0].id), q = map.affix(1, item.affixes[1].id);
			if ((p <= 0) && (q <= 0)) {
				problem = Name(item, n) + ": a magic item would be left with neither prefix nor suffix (no saved item has that, so what the game makes of it is not known)";
				return false;
			}
		}
		touched = touched || affixTouched;
		result.append(bytes, cursor, begin - cursor);
		if (!touched) {
			result.append(bytes, begin, end - begin);
			expect.stats = item.stats;
		} else {
			if (item.lists.empty()) {
				problem = Name(item, n) + ": it has stats but no stat list was recorded";
				return false;
			}
			BitOut bits;
			size_t from = item.bitStart;
			expect.affixes.clear();
			for (const AffixSlot& slot : item.affixes) {
				const int32_t to    = map.affix(slot.suffix, slot.id);
				const size_t  start = slot.flagged ? slot.bit - 1u : slot.bit;
				bits.copy(bytes, from, start);
				AffixSlot kept = slot;
				if (to < 0) {
					++out.affixesDropped;
					if (slot.flagged) {
						bits.put(0, 1); // the slot is empty now
					} else {
						bits.put(0, 11); // magic: 0 = none
						kept.id = 0;
						expect.affixes.push_back(kept);
					}
				} else {
					out.affixesRenumbered += to != slot.id ? 1 : 0;
					if (slot.flagged) {
						bits.put(1, 1);
					}
					bits.put(static_cast<uint32_t>(to), 11);
					kept.id = static_cast<uint16_t>(to);
					expect.affixes.push_back(kept);
				}
				from = slot.bit + 11u;
			}
			bits.copy(bytes, from, item.lists.front().first);
			for (size_t list = 0; list < item.lists.size(); ++list) {
				bool dropping = false;
				for (const StatValue& value : item.stats) {
					if (value.list != list) {
						continue;
					}
					if (!value.follower) {
						const int32_t to = map.to[value.stat];
						dropping         = to < 0;
						if (dropping) {
							++out.dropped;
							continue;
						}
						bits.put(static_cast<uint32_t>(to), 9);
						out.renumbered += to != value.stat ? 1 : 0;
					} else if (dropping) {
						++out.dropped;
						continue;
					}
					const StatShape& shape = map.shape[value.stat];
					const int64_t    raw   = static_cast<int64_t>(value.value) + shape.saveAdd;
					if (shape.paramBits < 32 && (static_cast<uint64_t>(value.param) >> shape.paramBits) != 0) {
						problem = Name(item, n) + ": stat " + std::to_string(value.stat) + " has param " + std::to_string(value.param) + ", which does not fit in " + std::to_string(shape.paramBits) + " save param bits";
						return false;
					}
					if (raw < 0 || (shape.saveBits < 64 && (static_cast<uint64_t>(raw) >> shape.saveBits) != 0)) {
						problem = Name(item, n) + ": stat " + std::to_string(value.stat) + " has the value " + std::to_string(value.value) + ", which does not fit in " + std::to_string(shape.saveBits) + " save bits with save add "
						          + std::to_string(shape.saveAdd) + " (it would be saved as " + std::to_string(raw) + ")";
						return false;
					}
					if (shape.paramBits != 0) {
						bits.put(value.param, shape.paramBits);
					}
					bits.put(static_cast<uint64_t>(raw), shape.saveBits);
					out.resized += map.resized[value.stat] != 0 ? 1 : 0;
					StatValue moved = value;
					moved.stat      = static_cast<uint16_t>(map.to[value.stat]);
					expect.stats.push_back(moved);
				}
				bits.put(0x1ff, 9);
			}
			bits.copy(bytes, item.lists.back().second, item.bitEnd);
			const std::string& made = bits.bytes;
			if (made != bytes.substr(begin, end - begin)) {
				++out.itemsChanged;
			}
			result += made;
			delta += static_cast<int64_t>(made.size()) - static_cast<int64_t>(end - begin);
		}
		ends.emplace_back(end, static_cast<size_t>(delta));
		out.expected.push_back(std::move(expect));
		cursor = end;
	}
	result.append(bytes, cursor, std::string::npos);
	const auto moved = [&](size_t old) { // where an old byte place is now (never inside an item)
		int64_t shift = 0;
		for (const auto& [end, total] : ends) {
			if (end <= old) {
				shift = static_cast<int64_t>(total);
			}
		}
		return static_cast<size_t>(static_cast<int64_t>(old) + shift);
	};
	if (stash) {
		for (size_t at = 0; at < bytes.size();) {
			const uint32_t length = Dword(bytes, at + 16);
			const size_t   now = moved(at), nowEnd = moved(at + length);
			PutDword(result, now + 16, static_cast<uint32_t>(nowEnd - now));
			at += length;
		}
	} else {
		PutDword(result, 8, static_cast<uint32_t>(result.size()));
		PutDword(result, 12, CharacterChecksum(result));
	}
	out.changed = result != bytes;
	out.bytes   = std::move(result);
	return true;
}

bool SameItems(const std::vector<Item>& expected, const std::vector<Item>& got, std::string& why) {
	if (expected.size() != got.size()) {
		why = std::to_string(got.size()) + " items read back, " + std::to_string(expected.size()) + " expected";
		return false;
	}
	for (size_t i = 0; i < expected.size(); ++i) {
		const Item &a = expected[i], &b = got[i];
		if (a.code != b.code || a.flags != b.flags || a.location != b.location || a.equipped != b.equipped || a.x != b.x || a.y != b.y || a.page != b.page || a.quality != b.quality || a.level != b.level
		    || a.inSocket != b.inSocket || a.quantity != b.quantity || a.section != b.section) {
			why = Name(a, i) + " reads back as another item (" + b.code + ", " + b.section + ")";
			return false;
		}
		bool sameAffixes = a.affixes.size() == b.affixes.size();
		for (size_t k = 0; sameAffixes && k < a.affixes.size(); ++k) {
			sameAffixes = a.affixes[k].suffix == b.affixes[k].suffix && a.affixes[k].slot == b.affixes[k].slot && a.affixes[k].flagged == b.affixes[k].flagged && a.affixes[k].id == b.affixes[k].id;
		}
		if (!sameAffixes) {
			why = Name(a, i) + ": its affixes do not read back as expected";
			return false;
		}
		if (a.stats.size() != b.stats.size()) {
			why = Name(a, i) + ": " + std::to_string(b.stats.size()) + " stats read back, " + std::to_string(a.stats.size()) + " expected";
			return false;
		}
		for (size_t s = 0; s < a.stats.size(); ++s) {
			const StatValue &x = a.stats[s], &y = b.stats[s];
			if (x.stat != y.stat || x.value != y.value || x.param != y.param || x.list != y.list || x.follower != y.follower) {
				why = Name(a, i) + ": stat " + std::to_string(x.stat) + " = " + std::to_string(x.value) + " (param " + std::to_string(x.param) + ") expected, stat " + std::to_string(y.stat) + " = "
				      + std::to_string(y.value) + " (param " + std::to_string(y.param) + ") read back";
				return false;
			}
		}
	}
	return true;
}

bool CheckRewrite(bool stash, const std::string& original, const FileResult& read, const std::string& bytes, const StatMap& map, const Rewrite& rewrite, std::string& why) {
	const FileResult back = ReadAs(stash, bytes, map.after);
	if (!back.ok) {
		why = "it does not read to its end with the table after the change: " + back.problem;
		return false;
	}
	if (!SameItems(rewrite.expected, back.items, why)) {
		return false;
	}
	if (!stash) {
		if (Dword(bytes, 8) != bytes.size() || Dword(bytes, 12) != CharacterChecksum(bytes)) {
			why = "its size or checksum field is wrong";
			return false;
		}
		if (read.attributesRead != back.attributesRead || read.attributes.size() != back.attributes.size()) {
			why = "its character attributes do not read back as before";
			return false;
		}
		for (size_t i = 0; i < read.attributes.size(); ++i) {
			if (read.attributes[i].stat != back.attributes[i].stat || read.attributes[i].value != back.attributes[i].value) {
				why = "character attribute " + std::to_string(read.attributes[i].stat) + " does not read back as before";
				return false;
			}
		}
	}
	const auto oldGaps = Gaps(read.items, original.size()), newGaps = Gaps(back.items, bytes.size());
	const std::set<size_t> oldFields = LengthFields(stash, original), newFields = LengthFields(stash, bytes);
	for (size_t g = 0; g < oldGaps.size(); ++g) {
		const auto [a0, a1] = oldGaps[g];
		const auto [b0, b1] = newGaps[g];
		if (a1 - a0 != b1 - b0) {
			why = "the bytes between items " + std::to_string(g) + " and " + std::to_string(g + 1) + " changed in length";
			return false;
		}
		for (size_t k = 0; k < a1 - a0; ++k) {
			if (original[a0 + k] != bytes[b0 + k] && (oldFields.count(a0 + k) == 0 || newFields.count(b0 + k) == 0)) {
				why = "byte " + std::to_string(b0 + k) + " outside the items changed (it is no size, checksum or section length)";
				return false;
			}
		}
	}
	return true;
}

bool ApplyBefore(Tables& tables, const std::vector<ResizeSpec>& before, std::string& why) {
	for (const ResizeSpec& shape : before) {
		if (shape.stat > kMaxId || shape.saveBits < 1 || shape.saveBits > 32 || shape.paramBits > 32) {
			why = "the saved shape given for stat " + std::to_string(shape.stat) + " is not one a save can hold (ids 0-510, save bits 1-32, param bits 0-32)";
			return false;
		}
		if (tables.stats.size() <= shape.stat) {
			tables.stats.resize(static_cast<size_t>(shape.stat) + 1);
		}
		const uint8_t csv           = tables.stats[shape.stat].csvBits;
		tables.stats[shape.stat]    = { shape.saveBits, shape.saveAdd, shape.paramBits, csv };
	}
	return true;
}

FixOut Fix(const std::vector<FixInput>& files, const std::vector<ChangeSpec>& changes, const std::vector<ResizeSpec>& resizes, bool dryRun, FixIo& io, const std::vector<AffixSpec>& affixes, int64_t notAfter) {
	FixOut out;
	out.dryRun = dryRun;
	std::vector<Rewrite>    rewrites(files.size());
	std::map<const Tables*, StatMap> maps;
	std::string             refusal;
	for (size_t i = 0; i < files.size(); ++i) {
		const FixInput& file = files[i];
		FixFileOut      report;
		report.name        = file.name;
		report.bytesBefore = report.bytesAfter = static_cast<int32_t>(file.bytes.size());
		std::string why;
		const FileResult read = file.tables != nullptr ? ReadAs(file.stash, file.bytes, *file.tables) : FileResult {};
		report.items          = static_cast<uint32_t>(read.items.size());
		if (notAfter != 0 && (file.writeTime == 0 || file.writeTime > notAfter)) {
			why = file.writeTime == 0 ? "when it was last written is not known, so it cannot be told apart from one saved with the table as it is now"
			                          : "it was saved after the table changed (" + std::to_string(file.writeTime) + " > " + std::to_string(notAfter) + "), so its stat numbers may already be the new ones and cannot be told apart";
		} else if (file.tables == nullptr || !read.ok) {
			why = "it cannot be read to its end, so what it holds is not known: " + (file.tables == nullptr ? std::string("no stat table") : read.problem);
			++out.unreadable;
		} else {
			auto found = maps.find(file.tables);
			if (found == maps.end()) {
				StatMap map;
				if (!MakeMap(*file.tables, changes, resizes, map, why, affixes)) {
					out.why = "The change cannot be made, nothing was written: " + why + ".";
					out.files.clear();
					return out; // a bad change list: said once, for every file
				}
				found = maps.emplace(file.tables, std::move(map)).first;
			}
			const StatMap& map = found->second;
			if (file.stash && !StashRecordTailSafe(file.bytes, why, report.note)) {
			} else if (!file.stash && Dword(file.bytes, 12) != CharacterChecksum(file.bytes)) {
				why = "its checksum is not the one worked out the known way, so it is not touched";
			} else if (!file.stash && !read.attributesRead) {
				why = "its character attributes cannot be read (" + read.attributesProblem + ")";
			} else {
				for (const StatValue& attribute : read.attributes) {
					if (why.empty() && map.touches(attribute.stat)) {
						why = "the character attribute stat " + std::to_string(attribute.stat) + " would change, and the writer does not change attributes";
					}
				}
			}
			if (why.empty() && RewriteFile(file.stash, file.bytes, read, map, rewrites[i], why) && rewrites[i].changed) {
				CheckRewrite(file.stash, file.bytes, read, rewrites[i].bytes, map, rewrites[i], why);
				if (!why.empty()) {
					why = "worked out in memory, it does not read back as expected: " + why;
				}
			}
		}
		if (!why.empty()) {
			report.state = FixState::Refused;
			report.why   = why;
			if (refusal.empty()) {
				refusal = file.name + ": " + why;
			}
		} else {
			const Rewrite& r    = rewrites[i];
			report.state        = r.changed ? FixState::WouldWrite : FixState::Unchanged;
			report.itemsChanged = r.itemsChanged;
			report.dropped      = r.dropped;
			report.renumbered   = r.renumbered;
			report.resized      = r.resized;
			report.affixesRenumbered = r.affixesRenumbered;
			report.affixesDropped    = r.affixesDropped;
			out.affixesRenumbered += r.affixesRenumbered;
			out.affixesDropped += r.affixesDropped;
			report.bytesAfter   = static_cast<int32_t>(r.bytes.size());
			out.changed += r.changed ? 1 : 0;
			out.itemsChanged += r.itemsChanged;
			out.dropped += r.dropped;
			out.renumbered += r.renumbered;
			out.resized += r.resized;
		}
		out.files.push_back(std::move(report));
	}
	if (!refusal.empty()) {
		out.why = "Nothing was written. " + refusal;
		return out;
	}
	if (dryRun) {
		out.ok = true;
		return out;
	}
	if (out.changed == 0) {
		out.ok = true;
		out.why = "no saved item holds a stat the change touches; nothing was written";
		return out;
	}
	std::string why;
	if (!io.mayWrite(why)) {
		out.why = "Nothing was written: " + why;
		for (FixFileOut& report : out.files) {
			report.state = report.state == FixState::WouldWrite ? FixState::Unchanged : report.state;
		}
		return out;
	}
	if (!io.backup(files, out.backup, why)) {
		out.why = "Nothing was written: the backup could not be made (" + why + ")";
		for (FixFileOut& report : out.files) {
			report.state = report.state == FixState::WouldWrite ? FixState::Unchanged : report.state;
		}
		return out;
	}
	std::vector<RestoreEntry> plan;
	for (size_t i = 0; i < files.size(); ++i) {
		if (rewrites[i].changed) {
			plan.push_back({ files[i].name, files[i].bytes, rewrites[i].bytes.size(), Fnv64(rewrites[i].bytes) });
		}
	}
	if (!io.planned(plan, why)) {
		out.why = "Nothing was written: what would be written could not be kept in the backup (" + why + ")";
		for (FixFileOut& report : out.files) {
			report.state = report.state == FixState::WouldWrite ? FixState::Unchanged : report.state;
		}
		return out;
	}
	std::vector<size_t> written;
	std::string         failure;
	for (size_t i = 0; i < files.size() && failure.empty(); ++i) {
		if (!rewrites[i].changed) {
			continue;
		}
		std::string back;
		if (!io.mayWrite(why)) {
			failure = "writing stopped before " + files[i].name + ": " + why;
			break;
		}
		written.push_back(i);
		if (!io.write(files[i].name, rewrites[i].bytes, why)) {
			failure = files[i].name + " could not be written (" + why + ")";
		} else if (!io.read(files[i].name, back) || back != rewrites[i].bytes) {
			failure = files[i].name + " does not read back from disk as it was written";
		} else {
			const FileResult read = ReadAs(files[i].stash, files[i].bytes, *files[i].tables);
			if (!CheckRewrite(files[i].stash, files[i].bytes, read, back, maps.at(files[i].tables), rewrites[i], why)) {
				failure = files[i].name + ", read back from disk: " + why;
			} else {
				out.files[i].state = FixState::Written;
			}
		}
	}
	if (failure.empty()) {
		out.ok = true;
		return out;
	}
	for (const size_t i : written) {
		std::string back, error;
		if (io.write(files[i].name, files[i].bytes, error) && io.read(files[i].name, back) && back == files[i].bytes) {
			out.files[i].state = FixState::Restored;
			out.files[i].why   = "put back from the backup: " + failure;
			++out.restored;
		} else {
			out.files[i].state = FixState::Refused;
			out.files[i].why   = "COULD NOT BE PUT BACK: copy it back by hand from " + out.backup;
			++out.notRestored;
		}
	}
	for (FixFileOut& report : out.files) {
		report.state = report.state == FixState::WouldWrite ? FixState::Unchanged : report.state;
	}
	out.why = out.notRestored == 0 ? "Every file is as it was (put back from the backup): " + failure
	                               : std::to_string(out.notRestored) + " file(s) COULD NOT BE PUT BACK - copy them back by hand from " + out.backup + ". The cause: " + failure;
	return out;
}

} // namespace savescan
