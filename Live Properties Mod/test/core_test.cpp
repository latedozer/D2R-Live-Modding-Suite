// prop_core against a port of the game's own linking routine (image 6ab3782c, 0x31edb3..0x31f0a3,
// written from its disassembly): a table the "game" linked with a property's row already in it
// must be byte for byte the table Wire() makes from one linked without it.
#include "../prop_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace prop;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
	g_failed += ok ? 0 : 1;
}

template <typename T>
static T Get(const uint8_t* at) {
	T v;
	memcpy(&v, at, sizeof(v));
	return v;
}
template <typename T>
static void Put(uint8_t* at, T v) {
	memcpy(at, &v, sizeof(v));
}

constexpr size_t kCount = 501;

// What the game does to the whole table after reading it.
static void GameLink(std::vector<uint8_t>& table) {
	uint8_t* rows = table.data();
	for (size_t i = 0; i < kCount; ++i) {
		memset(rows + i * kStatRow + 0x5a, 0xff, 0x80);
	}
	for (size_t i = 0; i < kCount; ++i) {
		uint8_t* row = rows + i * kStatRow;
		if (static_cast<uint8_t>(row[0x50] - 1) > 0xc) {
			row[0x50] = 0;
			continue;
		}
		const uint16_t base = Get<uint16_t>(row + 0x52);
		if (base < kCount) {
			uint8_t* a = rows + base * kStatRow;
			a[0x4d]    = 1;
			for (int k = 0; k < 0x40; ++k) {
				if (Get<uint16_t>(a + 0x5a + k * 2) >= kCount) {
					Put<uint16_t>(a + 0x5a + k * 2, static_cast<uint16_t>(i));
					break;
				}
			}
			if (static_cast<uint8_t>(row[0x50] - 4) <= 1) {
				row[0x4f] = 1;
			}
		}
		for (int j = 0; j < 3; ++j) {
			row[0x4d]             = 1;
			const uint16_t target = Get<uint16_t>(row + 0x54 + j * 2);
			if (target >= kCount) {
				break;
			}
			uint8_t* t = rows + target * kStatRow;
			for (int k = 0; k < 0x10; ++k) {
				if (t[0xde + k * 6] != 0) {
					continue;
				}
				Put<uint16_t>(t + 0xdc + k * 6, static_cast<uint16_t>(i));
				t[0xde + k * 6] = row[0x50];
				Put<uint16_t>(t + 0xda + k * 6, Get<uint16_t>(row + 0x52));
				t[0xdf + k * 6] = row[0x51];
				t[0x4e]         = 1;
				uint8_t flag    = 0;
				if (i == 7 || target == 7) {
					flag = 0x20;
				} else if (i == 9 || target == 9) {
					flag = 0x40;
				} else if (i == 11 || target == 11) {
					flag = 0x80;
				}
				if (flag != 0) {
					row[4] |= flag;
					row[4] |= 0x10;
				}
				break;
			}
		}
	}
}

// A table as the game reads it: blank cells are zero, blank stat names 0xffff, a few op stats.
static std::vector<uint8_t> Read() {
	std::vector<uint8_t> table(kCount * kStatRow, 0);
	for (size_t i = 0; i < kCount; ++i) {
		uint8_t* row = table.data() + i * kStatRow;
		Put<uint32_t>(row, static_cast<uint32_t>(i));
		Put<uint16_t>(row + 0x52, kNoStat);
		for (int j = 0; j < 3; ++j) {
			Put<uint16_t>(row + 0x54 + j * 2, kNoStat);
		}
		if (i < 375) {
			row[kSendBits] = 6, row[kSaveBits] = 6;
		}
	}
	const auto op = [&](int stat, int kind, int param, int base, int target) {
		uint8_t* row = table.data() + stat * kStatRow;
		row[0x50] = static_cast<uint8_t>(kind), row[0x51] = static_cast<uint8_t>(param);
		Put<uint16_t>(row + 0x52, static_cast<uint16_t>(base));
		Put<uint16_t>(row + 0x54, static_cast<uint16_t>(target));
	};
	op(216, 2, 3, 12, 7);    // item_hp_perlevel: of level, to maxhp
	op(217, 2, 3, 12, 9);    // item_mana_perlevel
	op(371, 2, 12, 7, 74);   // RegenCharm: of maxhp, to hpregen
	op(76, 11, 0, kNoStat, 7); // maxhp percent
	return table;
}

// The row a live property would be, had it been in the text file.
static void TextRow(std::vector<uint8_t>& table, int slot, int base, int target) {
	uint8_t* s = table.data() + slot * kStatRow;
	Put<uint32_t>(s + kFlags, kFlagSigned);
	s[kSendBits] = kBits, s[kSaveBits] = kBits, s[kSaveBits109] = kBits;
	s[0x50] = kOp, s[0x51] = kShift;
	Put<uint16_t>(s + 0x52, static_cast<uint16_t>(base));
	Put<uint16_t>(s + 0x54, static_cast<uint16_t>(target));
}

// GameLink() makes one entry (the row's own param); a live property has two more.
static void Extra(std::vector<uint8_t>& table, int slot, int base, int target) {
	uint8_t* t = table.data() + target * kStatRow;
	for (int n = 1; n < 3; ++n) {
		int k = 0;
		while (t[kOpList + k * 6 + 4] != 0) {
			++k;
		}
		Put<uint16_t>(t + kOpList + k * 6, static_cast<uint16_t>(base));
		Put<uint16_t>(t + kOpList + k * 6 + 2, static_cast<uint16_t>(slot));
		t[kOpList + k * 6 + 5] = kShifts[n];
		t[kOpList + k * 6 + 4] = kOp;
	}
}

static int Entries(const std::vector<uint8_t>& table, int target, int slot) {
	const uint8_t* t = table.data() + target * kStatRow;
	int            n = 0;
	for (int k = 0; k < kOpListMax && t[kOpList + k * 6 + 4] != 0; ++k) {
		n += Get<uint16_t>(t + kOpList + k * 6 + 2) == slot ? 1 : 0;
	}
	return n;
}

static bool Same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i] != b[i]) {
			printf("      first difference: stat %zu, offset 0x%zx: %02x / %02x\n", i / kStatRow, i % kStatRow, a[i], b[i]);
			return false;
		}
	}
	return true;
}

int main() {
	std::string problem;

	auto plain = Read();
	GameLink(plain);
	Check(IsBlank(plain.data() + 486 * kStatRow) && !IsBlank(plain.data() + 216 * kStatRow) && !IsBlank(plain.data() + 7 * kStatRow), "a filler row is blank, a stat in use and an op target are not");

	// one property: 486 = N% of maxmana (9) to hpregen (74)
	auto want = Read();
	TextRow(want, 486, 9, 74);
	GameLink(want);
	Extra(want, 486, 9, 74);
	auto got = plain;
	Check(Wire(got.data(), kCount, 486, 9, 74, problem) && Same(got, want), "wiring a filler row = what the game links from the text file");

	// to maxhp: the life flags
	want = Read();
	TextRow(want, 486, 9, 7);
	GameLink(want);
	Extra(want, 486, 9, 7);
	auto life = plain;
	Check(Wire(life.data(), kCount, 486, 9, 7, problem) && Same(life, want), "a property that adds to max life gets the game's life flags");

	// changing a wired property = as if the text had said the new thing
	Check(Wire(got.data(), kCount, 486, 9, 7, problem) && Same(got, want), "changing a property's stats leaves no trace of the old ones");

	// two properties on one target; unlinking in two steps; switching off
	got = plain;
	Check(Wire(got.data(), kCount, 485, 0, 74, problem) && Wire(got.data(), kCount, 486, 9, 74, problem) && Entries(got, 74, 485) == 3 && Entries(got, 74, 486) == 3 && Entries(got, 74, 371) == 1,
		"two properties adding to one stat: three entries each, the stat's own entry stays");
	uint16_t a = 0, t = 0;
	Check(IsWired(got.data() + 485 * kStatRow, &a, &t) && a == 0 && t == 74, "a wired row says what it is wired to");
	Unlink(got.data(), kCount, 485);
	Check(Entries(got, 74, 485) == 0 && Entries(got, 74, 486) == 3 && Entries(got, 74, 371) == 1 && IsWired(got.data() + 485 * kStatRow, &a, &t) && t == 74,
		"unlinking takes the entries out and the row still names its target");
	Check(Wire(got.data(), kCount, 485, 0, 7, problem) && Entries(got, 74, 485) == 0 && Entries(got, 7, 485) == 3, "an unlinked row can be wired to something else");
	Unwire(got.data(), kCount, 485);
	const uint8_t* off = got.data() + 485 * kStatRow;
	Check(Entries(got, 7, 485) == 0 && Entries(got, 74, 486) == 3 && Entries(got, 74, 371) == 1 && off[kOpAt] == 0 && off[kSaveBits] == kBits && off[kSendBits] == kBits
	      && Get<uint16_t>(got.data() + 0 * kStatRow + kDependents) == kNoStat && got[0 * kStatRow + kHasOp] == 0 && got[7 * kStatRow + kIsTarget] == 1,
		"switching a property off takes out its links (other entries stay) and keeps its 13 save bits");
	Check(IsOurs(got.data() + 485 * kStatRow) && !IsWired(got.data() + 485 * kStatRow, nullptr, nullptr), "a switched-off row is still the plugin's");
	Check(Wire(got.data(), kCount, 485, 0, 74, problem), "and can be wired again");

	// the text line
	got = plain;
	Check(Shape(got.data() + 486 * kStatRow, 1234, 60) && IsOurs(got.data() + 486 * kStatRow) && got[486 * kStatRow + kDescFunc] == kDescFuncPct
	      && Get<uint16_t>(got.data() + 486 * kStatRow + kDescStrPos) == 1234 && Wire(got.data(), kCount, 486, 9, 74, problem) && got[486 * kStatRow + kDescFunc] == kDescFuncPct,
		"a shaped row shows '+N% text', is the plugin's, and keeps its text when wired");
	Check(!Shape(got.data() + 216 * kStatRow, 1234, 60), "a row in use is not shaped");

	// refusals: nothing written
	got = plain;
	Check(!Wire(got.data(), kCount, 216, 9, 74, problem) && Same(got, plain), "a row in use is refused");
	Check(!Wire(got.data(), kCount, 486, 9, 9, problem) && !Wire(got.data(), kCount, 486, 486, 74, problem) && !Wire(got.data(), kCount, 486, 9, 600, problem) && Same(got, plain),
		"the same stat twice, its own stat, a number past the table: refused");
	got = plain;
	for (int n = 0; n < 14; ++n) { // 14 of hpregen's 16 places taken: no room for three
		uint8_t* entry = got.data() + 74 * kStatRow + kOpList + n * 6;
		entry[4]       = 2;
	}
	auto full = got;
	Check(!Wire(got.data(), kCount, 486, 9, 74, problem) && Same(got, full), "a stat with fewer than three free places: refused");

	// any stat's op, the game's way
	{
		const uint16_t none = kNoStat;
		got                 = plain;
		const uint16_t regen[3] = { 74, none, none };
		UnlinkOp(got.data(), kCount, 371);
		Check(Entries(got, 74, 371) == 0 && got[74 * kStatRow + kIsTarget] == 0 && LinkOp(got.data(), kCount, 371, 2, 12, 7, regen, false, problem) && Same(got, plain),
			"unlinking a stat the game linked and linking it again = the table the game made");
		want          = Read();
		uint8_t* made = want.data() + 400 * kStatRow;
		made[0x50] = 2, made[0x51] = 5;
		Put<uint16_t>(made + 0x52, 0);
		Put<uint16_t>(made + 0x54, 2);
		Put<uint16_t>(made + 0x56, 7);
		GameLink(want);
		got                    = plain;
		const uint16_t two[3]  = { 2, 7, none };
		Check(LinkOp(got.data(), kCount, 400, 2, 5, 0, two, false, problem) && Same(got, want), "an op given to an empty row (two targets, one of them max life) = what the game links from the text file");
		ClearOp(got.data(), kCount, 400);
		Check(Same(got, plain), "taking that op away again leaves the table as it was");
		const uint16_t self[3] = { 400, none, none }, far[3] = { 600, none, none };
		got                    = plain;
		Check(!LinkOp(got.data(), kCount, 400, 14, 0, 0, two, false, problem) && !LinkOp(got.data(), kCount, 400, 2, 0, 0, self, false, problem) && !LinkOp(got.data(), kCount, 400, 2, 0, 0, far, false, problem)
		      && !LinkOp(got.data(), kCount, 400, 2, 0, 400, two, false, problem) && Same(got, plain),
			"op 14, itself as a target or base, a target past the table: refused, nothing written");
		Check(LinkOp(got.data(), kCount, 400, 2, 5, 0, two, true, problem) && Same(got, plain), "a dry run writes nothing");
	}

	uint8_t property[kPropRow];
	FillProperty(property, 300, 486);
	Check(Get<uint16_t>(property) == 300 && property[0x18] == 1 && Get<uint16_t>(property + 0x20) == 486 && Get<uint16_t>(property + 0x22) == kNoStat && Get<uint16_t>(property + 0x2c) == kNoStat,
		"the property row: its number, func 1, the stat, no other stats");
	Check(kMaxPct < (1 << (kBits - 1)) && (1 << (12 - kShifts[0])) + (1 << (12 - kShifts[1])) + (1 << (12 - kShifts[2])) == 41, "the three divisions add up to 41/4096 = 1.0009%");

	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
