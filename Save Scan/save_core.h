// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Save Scan: the part that reads saved items. No game, no Windows: it works on file bytes and
// plain tables, so test/core_test.cpp can run it natively against copies of real saves.
//
// The item layout below is D2R 3.4 (save version 105, "Reign of the Warlock"), worked out from
// the user's own saves in November 2026 (README.md lists what was confirmed and what was not):
//
//   32 flags (bit 4 identified, 11 socketed, 16 ear, 21 simple, 24 personalized, 26 runeword,
//             28 "recorded": a block of 55 more bits at the very end)
//   3 version, 3 location, 4 equipped place, 4 x, 4 y, 3 page
//   the item code, Huffman coded, up to a space
//   simple item:  [2 bits when the item's table row has questdiffcheck = 1]
//                 1 bit: an id follows -> 128 bits
//                 1 bit: a quantity follows -> 8 bits
//   other item:   3 bits: items in its sockets (they follow it in the list)
//                 32 id, 7 level, 4 quality, 1 (+3) picture, 1 (+11) class-specific
//                 by quality: low/superior 3, normal charm 12, magic 22, set/unique 12,
//                             rare/crafted 16 + 6 x (1 + 11)
//                 runeword: 16;  personalized: 8-bit letters up to a 0;  tome: 5
//                 1 bit: an id follows -> 128 bits;  1 bit (meaning not known)
//                 armor: 11 defense;  armor and weapons: 8 max durability, then 9 current if not 0
//                 classic stackable: 9 quantity;  socketed: 4 sockets;  set: 5 bits list mask
//                 the stat list, one more per bit of the set mask, one more for a runeword
//                 1 bit: a quantity follows -> 8 bits;  flag 28: 55 bits
//   then on to the next whole byte.
//
// A stat list is: 9-bit stat number (0x1ff ends it), then for that stat - and for the one or
// two stats that always follow it (17; 48, 50, 52; 54 and 57 with two) - its param bits and
// its save bits, both from the stat table. That table is handed in; the plugin takes it from
// the game's memory, so stats changed or made at run time are read the way the game saves them.
//
// Nothing here guesses its way on: a list of items counts as read only when the reader ends
// exactly on the marker or section end that must follow it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savescan {

struct StatShape {
	uint8_t saveBits  = 0; // 0: the stat cannot be on a saved item
	int32_t saveAdd   = 0;
	uint8_t paramBits = 0;
};

enum : uint8_t { kMisc = 0, kArmor = 1, kWeapon = 2 };
struct ItemKind {
	uint8_t kind      = kMisc;
	bool    stackable = false; // the table's own "stackable" column (9-bit quantity)
	bool    questDiff = false; // questdiffcheck = 1
	bool    charm     = false; // type scha / mcha / lcha
	bool    tome      = false; // type book
};

struct Tables {
	std::vector<StatShape>                    stats;
	std::unordered_map<std::string, ItemKind> items;
};

// One ItemStatCost row of the game is 0x144 bytes; these are the places the reader needs.
constexpr size_t kStatRow = 0x144, kRowSaveBits = 0x15, kRowSaveAdd = 0x18, kRowSaveParam = 0x20;
void ShapesFromRows(Tables& tables, const uint8_t* rows, size_t count);
// armor.txt / weapons.txt / misc.txt as text; rows of several files add up.
void ReadItemTable(Tables& tables, std::string_view text, uint8_t kind);
// itemstatcost.txt as text (tests; the plugin uses ShapesFromRows).
void ShapesFromText(Tables& tables, std::string_view text);

struct StatValue {
	uint16_t stat;
	int32_t  value;
	uint32_t param;
};

struct Item {
	std::string code;
	uint32_t    flags    = 0;
	uint8_t     location = 0, equipped = 0, x = 0, y = 0, page = 0, quality = 0, level = 0;
	bool        inSocket = false; // sits in the item before it
	uint16_t    quantity = 0;
	uint32_t    offset   = 0;     // where it starts in the file
	std::string section;          // "items", "corpse", "mercenary", "golem", "stash tab 3", "stash materials"
	std::vector<StatValue> stats;
};

struct FileResult {
	bool              ok = false; // every list of the file was read to its end marker
	std::string       problem;    // why not
	std::vector<Item> items;      // what was read (all of it when ok)
};

FileResult ReadCharacter(const uint8_t* data, size_t size, const Tables& tables); // .d2s
FileResult ReadStash(const uint8_t* data, size_t size, const Tables& tables);     // .d2i

// "worn", "inventory", "stash", "cube", "belt", "in a socket" ...
std::string Where(const Item& item);

} // namespace savescan
