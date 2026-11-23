// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "prop_core.h"

#include <cstring>

namespace prop {

namespace {

template <typename T>
T Get(const uint8_t* at) {
	T value;
	memcpy(&value, at, sizeof(value));
	return value;
}
template <typename T>
void Put(uint8_t* at, T value) {
	memcpy(at, &value, sizeof(value));
}

uint8_t* Row(uint8_t* rows, size_t n) {
	return rows + n * kStatRow;
}

// The flags the game itself adds to an op's row when life, mana or stamina is involved
// (0x31f02c..0x31f076; the four values are the dwords at 0x1d996e0).
uint32_t OpFlags(uint16_t slot, uint16_t target) {
	for (const int stat : { kStatHp, kStatMana, kStatStamina }) {
		if (slot == stat || target == stat) {
			return 0x10u | (stat == kStatHp ? 0x20u : stat == kStatMana ? 0x40u : 0x80u);
		}
	}
	return 0;
}

int DependentCount(const uint8_t* row, size_t count) {
	int n = 0;
	while (n < kDependentMax && Get<uint16_t>(row + kDependents + n * 2) < count) {
		++n;
	}
	return n;
}

int OpListCount(const uint8_t* row) {
	int n = 0;
	while (n < kOpListMax && row[kOpList + n * 6 + 4] != 0) {
		++n;
	}
	return n;
}

} // namespace

bool IsBlank(const uint8_t* row) {
	return row[kOpAt] == 0 && row[kSendBits] == 0 && row[kSaveBits] == 0 && row[kDescFunc] == 0 && row[kHasOp] == 0 && row[kIsTarget] == 0 && (Get<uint32_t>(row + kFlags) == 0)
	    && row[kOpList + 4] == 0;
}

bool IsOurs(const uint8_t* row) {
	const uint32_t flags = Get<uint32_t>(row + kFlags) & ~0xf0u;
	return flags == kFlagSigned && row[kSendBits] == kBits && row[kSaveBits] == kBits && row[kValShift] == 0 && (row[kDescFunc] == 0 || row[kDescFunc] == kDescFuncPct)
	    && row[kIsTarget] == 0 && (row[kOpAt] == 0 || (row[kOpAt] == kOp && row[kOpParam] == kShift));
}

bool Shape(uint8_t* row, uint16_t stringId, uint16_t priority) {
	if (!IsBlank(row) && !IsOurs(row)) {
		return false;
	}
	Put<uint32_t>(row + kFlags, kFlagSigned | (Get<uint32_t>(row + kFlags) & 0xf0u));
	row[kSendBits]    = kBits;
	row[kSendParam]   = 0;
	row[kValShift]    = 0;
	row[kSaveBits]    = kBits;
	row[kSaveBits109] = kBits;
	Put<int32_t>(row + kSaveAdd, 0);
	Put<int32_t>(row + kSaveAdd109, 0);
	Put<int32_t>(row + kSaveParam, 0);
	Put<uint16_t>(row + kDescPriority, priority);
	row[kDescVal] = 1;
	Put<uint16_t>(row + kDescStrPos, stringId);
	Put<uint16_t>(row + kDescStrNeg, stringId);
	row[kDescFunc] = kDescFuncPct;
	return true;
}

bool IsWired(const uint8_t* row, uint16_t* base, uint16_t* target) {
	if (!IsOurs(row) || row[kOpAt] != kOp) {
		return false;
	}
	if (base != nullptr) {
		*base = Get<uint16_t>(row + kOpBase);
	}
	if (target != nullptr) {
		*target = Get<uint16_t>(row + kOpStat);
	}
	return true;
}

void Unlink(uint8_t* rows, size_t count, uint16_t slot) {
	uint16_t base = kNoStat, target = kNoStat;
	if (slot >= count || !IsWired(Row(rows, slot), &base, &target)) {
		return;
	}
	if (target < count) {
		uint8_t* t = Row(rows, target);
		for (int i = 0; i < OpListCount(t);) {
			uint8_t* entry = t + kOpList + i * 6;
			if (Get<uint16_t>(entry + 2) != slot) {
				++i;
				continue;
			}
			uint8_t* last = t + kOpList + (OpListCount(t) - 1) * 6;
			if (entry != last) {
				// the last entry takes the place; its op byte is written last
				const uint8_t op = last[4];
				entry[4]         = 0;
				memcpy(entry, last, 4);
				entry[5] = last[5];
				entry[4] = op;
			}
			memset(last, 0, 6);
		}
		if (OpListCount(t) == 0) {
			t[kIsTarget] = 0; // nothing is added to it any more
		}
	}
	if (base < count) {
		uint8_t*  a = Row(rows, base);
		const int n = DependentCount(a, count);
		for (int i = 0; i < n; ++i) {
			if (Get<uint16_t>(a + kDependents + i * 2) == slot) {
				Put<uint16_t>(a + kDependents + i * 2, Get<uint16_t>(a + kDependents + (n - 1) * 2));
				Put<uint16_t>(a + kDependents + (n - 1) * 2, kNoStat);
				if (n == 1 && a[kOpAt] == 0) {
					a[kHasOp] = 0; // no op of its own and nothing depends on it any more
				}
				break;
			}
		}
	}
}

void Unwire(uint8_t* rows, size_t count, uint16_t slot) {
	if (slot >= count || !IsWired(Row(rows, slot), nullptr, nullptr)) {
		return;
	}
	Unlink(rows, count, slot);
	uint8_t* s = Row(rows, slot);
	s[kOpAt]   = 0;
	Put<uint16_t>(s + kOpBase, kNoStat);
	Put<uint16_t>(s + kOpStat, kNoStat);
	Put<uint32_t>(s + kFlags, kFlagSigned);
}

bool Wire(uint8_t* rows, size_t count, uint16_t slot, uint16_t base, uint16_t target, std::string& problem) {
	if (slot >= count || base >= count || target >= count) {
		problem = "a stat number is past the end of the table";
		return false;
	}
	if (base == slot || target == slot || base == target) {
		problem = "the two stats must be different, and neither can be the property's own";
		return false;
	}
	uint8_t* s = Row(rows, slot);
	if (!IsBlank(s) && !IsOurs(s)) {
		problem = "the property's stat row is in use by something else";
		return false;
	}
	uint8_t* a = Row(rows, base);
	uint8_t* t = Row(rows, target);
	int      inA = 0, inT = 0; // places the property already holds there (it may be linked, half linked or not)
	for (int i = 0; i < DependentCount(a, count); ++i) {
		inA += Get<uint16_t>(a + kDependents + i * 2) == slot ? 1 : 0;
	}
	for (int i = 0; i < OpListCount(t); ++i) {
		inT += Get<uint16_t>(t + kOpList + i * 6 + 2) == slot ? 1 : 0;
	}
	if (DependentCount(a, count) - inA >= kDependentMax) {
		problem = "the stat the percentage is taken of already has 64 stats depending on it";
		return false;
	}
	if (OpListCount(t) - inT + 3 > kOpListMax) {
		problem = "the stat it is added to has no room for three more additions (16 at most; a live property takes three)";
		return false;
	}
	Unwire(rows, count, slot);

	// the row itself: save shape first, the op last
	Put<uint32_t>(s + kFlags, kFlagSigned | OpFlags(slot, target));
	s[kSendBits]    = kBits;
	s[kSendParam]   = 0;
	s[kValShift]    = 0;
	s[kSaveBits]    = kBits;
	s[kSaveBits109] = kBits;
	Put<int32_t>(s + kSaveAdd, 0);
	Put<int32_t>(s + kSaveAdd109, 0);
	Put<int32_t>(s + kSaveParam, 0);
	s[kOpParam] = kShift;
	Put<uint16_t>(s + kOpBase, base);
	Put<uint16_t>(s + kOpStat, target);
	Put<uint16_t>(s + kOpStat + 2, kNoStat);
	Put<uint16_t>(s + kOpStat + 4, kNoStat);
	s[kOpAt]  = kOp;
	s[kHasOp] = 1;

	// the stat the percentage is taken of: this row depends on it
	a[kHasOp] = 1;
	Put<uint16_t>(a + kDependents + DependentCount(a, count) * 2, slot);

	// the stat it is added to: three more entries, each op byte last (that byte is what ends the list)
	for (const uint8_t shift : kShifts) {
		uint8_t* entry = t + kOpList + OpListCount(t) * 6;
		Put<uint16_t>(entry, base);
		Put<uint16_t>(entry + 2, slot);
		entry[5] = shift;
		entry[4] = kOp;
	}
	t[kIsTarget] = 1;
	return true;
}

void UnlinkOp(uint8_t* rows, size_t count, uint16_t slot) {
	for (size_t n = 0; n < count; ++n) {
		uint8_t* t    = Row(rows, n);
		bool     took = false;
		for (int i = 0; i < OpListCount(t);) {
			uint8_t* entry = t + kOpList + i * 6;
			if (Get<uint16_t>(entry + 2) != slot) {
				++i;
				continue;
			}
			uint8_t* last = t + kOpList + (OpListCount(t) - 1) * 6;
			if (entry != last) {
				const uint8_t op = last[4];
				entry[4]         = 0;
				memcpy(entry, last, 4);
				entry[5] = last[5];
				entry[4] = op;
			}
			memset(last, 0, 6);
			took = true;
		}
		if (took && OpListCount(t) == 0) {
			t[kIsTarget] = 0;
		}
		const int deps = DependentCount(t, count);
		for (int i = 0; i < deps; ++i) {
			if (Get<uint16_t>(t + kDependents + i * 2) == slot) {
				Put<uint16_t>(t + kDependents + i * 2, Get<uint16_t>(t + kDependents + (deps - 1) * 2));
				Put<uint16_t>(t + kDependents + (deps - 1) * 2, kNoStat);
				if (deps == 1 && t[kOpAt] == 0) {
					t[kHasOp] = 0;
				}
				break;
			}
		}
	}
}

void ClearOp(uint8_t* rows, size_t count, uint16_t slot) {
	if (slot >= count) {
		return;
	}
	UnlinkOp(rows, count, slot);
	uint8_t* s  = Row(rows, slot);
	s[kOpAt]    = 0;
	s[kOpParam] = 0;
	s[0x4f]     = 0;
	Put<uint16_t>(s + kOpBase, kNoStat);
	for (int j = 0; j < 3; ++j) {
		Put<uint16_t>(s + kOpStat + j * 2, kNoStat);
	}
	Put<uint32_t>(s + kFlags, Get<uint32_t>(s + kFlags) & ~0xf0u);
	s[kHasOp] = DependentCount(s, count) != 0 ? 1 : 0;
}

bool LinkOp(uint8_t* rows, size_t count, uint16_t slot, uint8_t op, uint8_t param, uint16_t base, const uint16_t targets[3], bool dry, std::string& problem) {
	if (slot >= count) {
		problem = "the stat's number is past the end of the table";
		return false;
	}
	if (op < 1 || op > 13) {
		problem = "op is a number from 1 to 13";
		return false;
	}
	if (base != kNoStat && (base >= count || base == slot)) {
		problem = "op base must be another stat of the table (or empty)";
		return false;
	}
	int used = 0; // targets up to the first empty one, as the game reads them
	while (used < 3 && targets[used] != kNoStat) {
		if (targets[used] >= count || targets[used] == slot) {
			problem = "an op stat must be another stat of the table";
			return false;
		}
		++used;
	}
	if (base != kNoStat) {
		const uint8_t* a   = Row(rows, base);
		int            own = 0;
		for (int i = 0; i < DependentCount(a, count); ++i) {
			own += Get<uint16_t>(a + kDependents + i * 2) == slot ? 1 : 0;
		}
		if (DependentCount(a, count) - own >= kDependentMax) {
			problem = "op base already has 64 stats worked out from it";
			return false;
		}
	}
	for (int j = 0; j < used; ++j) {
		const uint8_t* t    = Row(rows, targets[j]);
		int            own  = 0, need = 0;
		for (int i = 0; i < OpListCount(t); ++i) {
			own += Get<uint16_t>(t + kOpList + i * 6 + 2) == slot ? 1 : 0;
		}
		for (int k = 0; k < used; ++k) {
			need += targets[k] == targets[j] ? 1 : 0;
		}
		if (OpListCount(t) - own + need > kOpListMax) {
			problem = "stat " + std::to_string(targets[j]) + " already takes 16 additions; there is no room for one more";
			return false;
		}
	}
	if (dry) {
		return true;
	}
	UnlinkOp(rows, count, slot);
	uint8_t* s = Row(rows, slot);
	uint32_t flags = Get<uint32_t>(s + kFlags) & ~0xf0u;
	for (int j = 0; j < used; ++j) {
		flags |= OpFlags(slot, targets[j]);
	}
	Put<uint32_t>(s + kFlags, flags);
	s[kOpParam] = param;
	Put<uint16_t>(s + kOpBase, base);
	for (int j = 0; j < 3; ++j) {
		Put<uint16_t>(s + kOpStat + j * 2, j < used ? targets[j] : kNoStat);
	}
	s[0x4f]   = base != kNoStat && (op == 4 || op == 5) ? 1 : 0;
	s[kOpAt]  = op;
	s[kHasOp] = 1;
	if (base != kNoStat) {
		uint8_t* a = Row(rows, base);
		a[kHasOp]  = 1;
		Put<uint16_t>(a + kDependents + DependentCount(a, count) * 2, slot);
	}
	for (int j = 0; j < used; ++j) {
		uint8_t* t     = Row(rows, targets[j]);
		uint8_t* entry = t + kOpList + OpListCount(t) * 6;
		Put<uint16_t>(entry, base);
		Put<uint16_t>(entry + 2, slot);
		entry[5]     = param;
		entry[4]     = op;
		t[kIsTarget] = 1;
	}
	return true;
}

void FillProperty(uint8_t* row, uint16_t id, uint16_t stat) {
	memset(row, 0, kPropRow);
	Put<uint16_t>(row, id);
	row[0x18] = 1; // func1: set the stat to the rolled value
	Put<uint16_t>(row + 0x20, stat);
	for (int i = 1; i < 7; ++i) {
		Put<uint16_t>(row + 0x20 + i * 2, kNoStat);
	}
}

} // namespace prop
