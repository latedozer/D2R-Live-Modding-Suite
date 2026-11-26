// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The op formulas of the stat card's "How the game sees it" page, on examples the game's own data
// fixes: per-level stats keep 8 per +1 with op param 3; RegenCharm's op 2 / param 12 on maxhp.
#include "../op_core.h"

#include <cstdio>

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

int main() {
	// item_armor_perlevel (op 4, param 3, base level): value 8 = +1 defense per character level
	const op::Result a = op::Apply(4, 3, 8, 10, 0);
	Check(a.known && a.add == 10, "per level: value 8, param 3, level 10 -> +10 (" + a.how + ")");
	Check(op::Apply(2, 3, 12, 85, 0).add == 127, "stat477 as the user set it: op 2, param 3, base level 85, value 12 -> +127 vitality");
	Check(op::Apply(2, 3, 3, 85, 0).add == 31, "value 3 at level 85 -> 3 x 85 / 8 = 31 (rounded down, as the game shifts)");
	// RegenCharm: op 2, param 12, base maxhp (kept x256), value N*41: about N% of max life
	const int64_t maxhp = 500 * 256; // 500 life
	Check(op::Apply(2, 12, 5 * 41, maxhp, 0).add == (5 * 41 * maxhp) >> 12, "RegenCharm: 5% (value 205), 500 life kept x256 -> " + std::to_string((5 * 41 * maxhp) >> 12));
	// percentage ops
	Check(op::Apply(1, 0, 20, 0, 300).add == 60, "op 1: 20% of 300 -> +60");
	const op::Result p = op::Apply(3, 3, 8, 10, 200);
	Check(p.add == 20, "op 3: (8 x 10 / 8)% = 10% of 200 -> +20 (" + p.how + ")");
	Check(!op::Apply(6, 0, 5, 0, 100).known && !op::Apply(9, 0, 5, 0, 100).known, "ops 6-9 depend on time / class: not worked out, said so");
	Check(op::Words(2, 3, "level", "vitality") == "adds value x level / 8 to vitality" && op::Words(0, 0, "", "") == "no op: the value is just kept", "words: op 2 / param 3 reads 'adds value x level / 8 to vitality'");
	Check(op::Apply(2, 40, 1, 1, 0).add == 0 && op::Apply(2, -1, 3, 4, 0).add == 12, "a param out of 0..31 is clamped (no undefined shift)");
	printf(g_failed == 0 ? "op: all passed\n" : "op: %d FAILED\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
