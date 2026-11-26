// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// A stat's op, worked out as the game's data guide describes it (ItemStatCost.js "op"), for the stat
// card's "How the game sees it" page: what a value adds to the op stat. The data guide writes op 2-5
// as "<< op param"; the game divides (>>), as RegenCharm and the per-level stats show (value 8 with
// param 3 = +1 per level). Tested natively (test/op_test.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace op {

struct Result {
	bool        known = false; // false: the op depends on what this page can't see (time of day, char stats)
	int64_t     add   = 0;     // what the value adds to the op stat (in the op stat's own kept units)
	std::string how;           // the formula with the numbers, for the page
};

// value: this stat's value; base: the op base stat's value (level, maxhp ...; op 4/5: on the item);
// target: the op stat's value before (ops 1, 3, 5, 11, 13 work on a percentage of it).
Result Apply(int op, int param, int64_t value, int64_t base, int64_t target);

// The op in words, e.g. "adds value x level / 8 to vitality".
std::string Words(int op, int param, const std::string& base, const std::string& target);

} // namespace op
