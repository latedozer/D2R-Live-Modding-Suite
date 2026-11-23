// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties: the game's magic affix rows (MagicPrefix, MagicSuffix, AutoMagic) as cells
// to show and type in. No game calls here, so it is tested natively (test/affix_test.cpp).
//
// Found in game build 0x6ab3782c (3.4.95041), the loader at 0x3195c0: the three tables are
// read with rows of 0x8c bytes and copied into one vector at data tables + 0x15e8 (count
// +0x15f0): the suffixes first, then the prefixes (their first row's address at +0x1608), then
// AutoMagic (+0x1610). The column list the loader is given says where each column goes:
//   +0x00 name (the string key, at most 31 letters)   +0x20 the key's string number (worked out after reading)
//   +0x22 version (word)
//   +0x24 + 0x10 * n  mod n+1: code (dword, a property row), param, min, max (dwords)
//   +0x54 spawnable  +0x56 transformcolor  +0x58 level  +0x5c group  +0x60 maxlevel
//   +0x64 rare  +0x65 levelreq  +0x66 classspecific  +0x67 class  +0x68 classlevelreq   (bytes)
//   +0x6a itype1..7, +0x78 etype1..5 (words, item type rows)  +0x82 frequency (byte)
//   +0x84 multiply  +0x88 add (dwords)
// The loader's column kinds: 1 text, 2 dword, 3 word, 4 byte, 0xd / 0xf / 0x1c looked up by name.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace affix {

constexpr size_t kRow = 0x8c, kNameRoom = 0x20, kNameId = 0x20;

enum Kind { kNumber, kSigned, kProp, kType, kName };

struct Field {
	const char* name;
	uint8_t     offset, size; // size 1, 2 or 4 bytes
	Kind        kind;
	long        least, most;
	const char* tip;
};

constexpr int kCells = 35, kNameCell = 0, kSpawnCell = 1, kLevelCell = 3, kMaxLevelCell = 4, kFrequencyCell = 6, kModCell = 11, kTypeCell = 23;
extern const Field kFields[kCells];
using Cells = std::array<std::string, kCells>;

// What the plugin knows about the game's names: property codes and item type codes by row.
// Empty cells hold what the game's own empty cells hold (learnt from the table: Learn).
struct Names {
	const std::string* props     = nullptr; // property code by row ("" = none)
	size_t             propCount = 0;
	const std::string* types     = nullptr; // item type code by row
	size_t             typeCount = 0;
	long               noProp = -1, noType = 0; // what an empty mod code / item type cell holds
};

long  Get(const uint8_t* row, const Field& field);
void  Put(uint8_t* row, const Field& field, long value);
Cells Read(const uint8_t* row, const Names& names);
// The cells (the name cell aside: the plugin puts the name) into `next`, a copy of the row.
// Everything is checked first; on false `next` may be half written and `problem` says why.
bool Build(const Cells& cells, const Names& names, uint8_t* next, std::string& problem);
// A row no affix uses: no name and no mod.
bool IsBlank(const uint8_t* row, const Names& names);
// What the game's empty cells hold, from the rows of a table: the most common mod 3 code and
// the most common etype5 (nearly every row leaves those empty).
void Learn(const uint8_t* rows, size_t count, Names& names);

} // namespace affix
