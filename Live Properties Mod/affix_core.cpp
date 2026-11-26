// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties: magic affix rows as cells (see affix_core.h).
#include "affix_core.h"

#include <cstdlib>
#include <cstring>
#include <map>

namespace affix {

const Field kFields[kCells] = {
	{ "name", 0x00, 0, kName, 0, 0, "The affix's name on the item: one of the game's string keys (Stout), or your own words (the plugin gives them a string of its own)" },
	{ "spawnable", 0x54, 1, kNumber, 0, 1, "1: the game may put it on items - also needed for a recipe's pre= / suf= to take it; 0: never" },
	{ "rare", 0x64, 1, kNumber, 0, 1, "1: it may also be on rare items" },
	{ "level", 0x58, 4, kNumber, 0, 1000, "The lowest affix level an item needs for it (drops: from the monster's level; a recipe: its lvl column). 99 keeps it off nearly all drops" },
	{ "maxlevel", 0x60, 4, kNumber, 0, 1000, "The highest affix level it comes on (empty: no top)" },
	{ "levelreq", 0x65, 1, kNumber, 0, 255, "The character level needed to use an item with it" },
	{ "frequency", 0x82, 1, kNumber, 0, 255, "How often it is picked against the others that fit (0: never, also not by a recipe's pre= / suf=). Most affixes: 1-4" },
	{ "group", 0x5c, 4, kNumber, 0, 1000000, "Affixes of the same group are never on one item together" },
	{ "classspecific", 0x66, 1, kNumber, 0, 255, "The class it is only for (a row of the class table; as the game read it)" },
	{ "class", 0x67, 1, kNumber, 0, 255, "With classspecific: the class whose items get the other level requirement" },
	{ "classlevelreq", 0x68, 1, kNumber, 0, 255, "The level requirement for that class" },
	{ "mod1code", 0x24, 4, kProp, 0, 0, "Property of the first mod (a code from the Properties tab)" },
	{ "mod1param", 0x28, 4, kSigned, -2147483647, 2147483647, "Its param" },
	{ "mod1min", 0x2c, 4, kSigned, -2147483647, 2147483647, "Its lowest roll" },
	{ "mod1max", 0x30, 4, kSigned, -2147483647, 2147483647, "Its highest roll" },
	{ "mod2code", 0x34, 4, kProp, 0, 0, "Property of the second mod" },
	{ "mod2param", 0x38, 4, kSigned, -2147483647, 2147483647, "Its param" },
	{ "mod2min", 0x3c, 4, kSigned, -2147483647, 2147483647, "Its lowest roll" },
	{ "mod2max", 0x40, 4, kSigned, -2147483647, 2147483647, "Its highest roll" },
	{ "mod3code", 0x44, 4, kProp, 0, 0, "Property of the third mod" },
	{ "mod3param", 0x48, 4, kSigned, -2147483647, 2147483647, "Its param" },
	{ "mod3min", 0x4c, 4, kSigned, -2147483647, 2147483647, "Its lowest roll" },
	{ "mod3max", 0x50, 4, kSigned, -2147483647, 2147483647, "Its highest roll" },
	{ "itype1", 0x6a, 2, kType, 0, 0, "An item type it may be on (scha small charm, char any charm, ring, amul...)" },
	{ "itype2", 0x6c, 2, kType, 0, 0, "Another item type it may be on" },
	{ "itype3", 0x6e, 2, kType, 0, 0, "Another item type it may be on" },
	{ "itype4", 0x70, 2, kType, 0, 0, "Another item type it may be on" },
	{ "itype5", 0x72, 2, kType, 0, 0, "Another item type it may be on" },
	{ "itype6", 0x74, 2, kType, 0, 0, "Another item type it may be on" },
	{ "itype7", 0x76, 2, kType, 0, 0, "Another item type it may be on" },
	{ "etype1", 0x78, 2, kType, 0, 0, "An item type it is never on" },
	{ "etype2", 0x7a, 2, kType, 0, 0, "An item type it is never on" },
	{ "etype3", 0x7c, 2, kType, 0, 0, "An item type it is never on" },
	{ "etype4", 0x7e, 2, kType, 0, 0, "An item type it is never on" },
	{ "etype5", 0x80, 2, kType, 0, 0, "An item type it is never on" },
};

long Get(const uint8_t* row, const Field& field) {
	if (field.size == 1) {
		return row[field.offset];
	}
	if (field.size == 2) {
		uint16_t value;
		memcpy(&value, row + field.offset, 2);
		return value;
	}
	int32_t value;
	memcpy(&value, row + field.offset, 4);
	return value;
}

void Put(uint8_t* row, const Field& field, long value) {
	if (field.size == 1) {
		row[field.offset] = static_cast<uint8_t>(value);
	} else if (field.size == 2) {
		const uint16_t v = static_cast<uint16_t>(value);
		memcpy(row + field.offset, &v, 2);
	} else {
		const int32_t v = static_cast<int32_t>(value);
		memcpy(row + field.offset, &v, 4);
	}
}

namespace {

const std::string* Data(const std::vector<std::string>* list) {
	return list != nullptr ? list->data() : nullptr;
}
size_t Size(const std::vector<std::string>* list) {
	return list != nullptr ? list->size() : 0;
}

std::string Word(const std::string* list, size_t count, long value, long none) {
	if (value == none) {
		return {};
	}
	return value >= 0 && static_cast<size_t>(value) < count && !list[value].empty() ? list[value] : "#" + std::to_string(value);
}

bool Number(const std::string& text, long least, long most, long& number) {
	number = 0;
	if (text.empty()) {
		return true;
	}
	char* end = nullptr;
	number    = strtol(text.c_str(), &end, 10);
	return end != text.c_str() && *end == '\0' && number >= least && number <= most;
}

// A code (or "#row") back to its row; "" is the empty cell.
bool Lookup(const std::string& text, const std::string* list, size_t count, long none, long& value) {
	if (text.empty() || text == "-") {
		value = none;
		return true;
	}
	if (text[0] == '#') {
		long row = 0;
		if (Number(text.substr(1), 0, static_cast<long>(count) - 1, row) && text.size() > 1) {
			value = row;
			return true;
		}
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		if (list[i] == text) {
			value = static_cast<long>(i);
			return true;
		}
	}
	return false;
}

} // namespace

Cells Read(const uint8_t* row, const Names& names) {
	Cells cells;
	for (int i = 0; i < kCells; ++i) {
		const Field& f = kFields[i];
		const long   v = f.kind == kName ? 0 : Get(row, f);
		std::string& c = cells[static_cast<size_t>(i)];
		switch (f.kind) {
		case kName:
			c.assign(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), kNameRoom - 1));
			break;
		case kProp:
			c = Word(Data(names.props), Size(names.props), v, names.noProp);
			break;
		case kType:
			c = Word(Data(names.types), Size(names.types), v, names.noType);
			break;
		default:
			c = v != 0 ? std::to_string(v) : "";
		}
	}
	return cells;
}

bool Build(const Cells& cells, const Names& names, uint8_t* next, std::string& problem) {
	for (int i = 0; i < kCells; ++i) {
		const Field&       f    = kFields[i];
		const std::string& text = cells[static_cast<size_t>(i)] == "-" ? std::string() : cells[static_cast<size_t>(i)];
		long               value = 0;
		switch (f.kind) {
		case kName:
			continue;
		case kProp:
			if (!Lookup(text, Data(names.props), Size(names.props), names.noProp, value)) {
				problem = std::string(f.name) + ": there is no property '" + text + "' (see the Properties tab)";
				return false;
			}
			break;
		case kType:
			if (!Lookup(text, Data(names.types), Size(names.types), names.noType, value)) {
				problem = std::string(f.name) + ": there is no item type '" + text + "' (codes as in itemtypes.txt: scha, char, ring...)";
				return false;
			}
			break;
		default:
			if (!Number(text, f.least, f.most, value)) {
				problem = std::string(f.name) + " is a whole number from " + std::to_string(f.least) + " to " + std::to_string(f.most);
				return false;
			}
		}
		Put(next, f, value);
	}
	// no min <= max check: the game's own rows have ranges like min -25, max -40
	return true;
}

size_t ClassicEnd(const uint8_t* rows, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		uint16_t version = 0;
		memcpy(&version, rows + i * kRow + 0x22, 2);
		if (version >= 1) {
			return i;
		}
	}
	return 0;
}

void Off(uint8_t* row) {
	Put(row, kFields[kSpawnCell], 0);
	Put(row, kFields[kFrequencyCell], 0);
}
bool IsOff(const uint8_t* row) {
	return Get(row, kFields[kSpawnCell]) == 0 && Get(row, kFields[kFrequencyCell]) == 0;
}
void On(uint8_t* row, const uint8_t* from) {
	Put(row, kFields[kSpawnCell], Get(from, kFields[kSpawnCell]));
	Put(row, kFields[kFrequencyCell], Get(from, kFields[kFrequencyCell]));
}

bool IsBlank(const uint8_t* row, const Names& names) {
	if (row[0] != 0) {
		return false;
	}
	for (int m = 0; m < 3; ++m) {
		if (Get(row, kFields[kModCell + m * 4]) != names.noProp) {
			return false;
		}
	}
	return true;
}

void Learn(const uint8_t* rows, size_t count, Names& names) {
	std::map<long, size_t> props, types;
	for (size_t i = 0; i < count; ++i) {
		++props[Get(rows + i * kRow, kFields[kModCell + 8])];
		++types[Get(rows + i * kRow, kFields[kTypeCell + 11])];
	}
	size_t most = 0;
	for (const auto& [value, seen] : props) {
		if (seen > most) {
			most = seen, names.noProp = value;
		}
	}
	most = 0;
	for (const auto& [value, seen] : types) {
		if (seen > most) {
			most = seen, names.noType = value;
		}
	}
}

} // namespace affix
