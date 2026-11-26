// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "op_core.h"

namespace op {
namespace {

std::string Num(int64_t v) {
	return std::to_string(v);
}

} // namespace

Result Apply(int op, int param, int64_t value, int64_t base, int64_t target) {
	Result r;
	const int shift = param < 0 ? 0 : param > 31 ? 31 : param;
	switch (op) {
	case 0:
		r.known = true;
		r.how   = "no op";
		break;
	case 1:
	case 11:
	case 13:
		r.known = true;
		r.add   = target * value / 100;
		r.how   = Num(target) + " x " + Num(value) + " / 100 = " + Num(r.add);
		break;
	case 2:
	case 4:
		r.known = true;
		r.add   = (value * base) >> shift;
		r.how   = Num(value) + " x " + Num(base) + " / " + Num(int64_t(1) << shift) + " = " + Num(r.add);
		break;
	case 3:
	case 5: {
		r.known             = true;
		const int64_t share = (value * base) >> shift;
		r.add               = target * share / 100;
		r.how               = Num(value) + " x " + Num(base) + " / " + Num(int64_t(1) << shift) + " = " + Num(share) + "%, of " + Num(target) + " = " + Num(r.add);
		break;
	}
	default:
		r.how = op == 6 || op == 7 ? "depends on the game's time of day" : op == 8 || op == 9 ? "depends on the character class (charstats)" : "does nothing";
		break;
	}
	return r;
}

std::string Words(int op, int param, const std::string& base, const std::string& target) {
	const std::string by = param > 0 ? " / " + std::to_string(int64_t(1) << (param > 31 ? 31 : param)) : "";
	switch (op) {
	case 0: return "no op: the value is just kept";
	case 1: return "adds value % of " + target + " to " + target;
	case 2: return "adds value x " + base + by + " to " + target;
	case 3: return "adds (value x " + base + by + ")% of " + target + " to " + target;
	case 4: return "adds value x the item's " + base + by + " to " + target;
	case 5: return "adds (value x the item's " + base + by + ")% of " + target + " to " + target;
	case 6: return "adds to " + target + " by the time of day";
	case 7: return "adds a percentage of " + target + " by the time of day";
	case 8: return "energy: " + target + " from the class's mana per energy";
	case 9: return "vitality: " + target + " from the class's life (or stamina) per vitality";
	case 11: return "adds value % of " + target + " to " + target + " (units)";
	case 13: return "adds value % of " + target + " to " + target + " (items)";
	default: return "op " + std::to_string(op) + ": does nothing";
	}
}

} // namespace op
