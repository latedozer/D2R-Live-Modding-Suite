// Live Properties: the part that knows the game's table rows. No game, no Windows: it works on
// plain row memory, so test/core_test.cpp can run it natively.
//
// A live property is "N% of stat A is added to stat B". The game already has that mechanism:
// an ItemStatCost row with op 2 adds (its value * A) >> op param to B. After reading the table
// the game links such rows into the rows of A and B (0x31edb3..0x31f0a3 in image 6ab3782c):
//   * A's row: a list of up to 64 stat numbers at +0x5a that depend on A (0xffff = end);
//   * B's row: a list of up to 16 entries {u16 A, u16 the op's stat, u8 op, u8 param} at +0xda
//     (op 0 = end), read by the stat code whenever B is worked out.
// Wire() makes the same links by hand for a row that was an unused filler; Unwire() takes them
// out again. Nothing else in the row's place changes, so the game needs no reload.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace prop {

constexpr size_t kStatRow = 0x144; // one ItemStatCost row
constexpr size_t kPropRow = 0x30;  // one Properties row

// The shape every live property's stat has: its value is N itself, in 13 signed bits. The game
// can only divide by powers of two, so the stat it is added to gets THREE entries for one
// property: N * A / 4096 + N * A / 512 + N * A / 128 = N * A * 41 / 4096 = 1.0009% of A per point.
constexpr uint8_t kShifts[3] = { 12, 9, 7 }; // the first is also the row's own op param
constexpr uint8_t kShift     = kShifts[0];
constexpr uint8_t kBits      = 13;
constexpr int     kMaxPct    = 500;
constexpr uint8_t kOp        = 2;
constexpr uint16_t kNoStat   = 0xffff;
constexpr uint8_t kDescFuncPct = 4; // "+N% <text>", with descval 1 (RegenCharm's display row)

constexpr int kStatHp = 7, kStatMana = 9, kStatStamina = 11;

// offsets in an ItemStatCost row (the game's own column table at 0x31e300)
constexpr size_t kFlags = 0x04, kSendBits = 0x08, kSendParam = 0x09, kValShift = 0x14, kSaveBits = 0x15, kSaveBits109 = 0x16, kSaveAdd = 0x18, kSaveAdd109 = 0x1c,
                 kSaveParam = 0x20, kDescPriority = 0x30, kDescFunc = 0x32, kDescVal = 0x33, kDescStrPos = 0x34, kDescStrNeg = 0x36, kHasOp = 0x4d, kIsTarget = 0x4e, kOpAt = 0x50, kOpParam = 0x51, kOpBase = 0x52, kOpStat = 0x54, kDependents = 0x5a,
                 kOpList = 0xda;
constexpr int    kDependentMax = 64, kOpListMax = 16;
constexpr uint32_t kFlagSigned = 1u << 8;

// A row no stat uses (Exalted Items' "xlt_unused_<n>" fillers are like this).
bool IsBlank(const uint8_t* row);
// A row Wire() made (wired or switched off), so it may be wired again.
bool IsOurs(const uint8_t* row);
// Is the row wired right now? Then base and target are what it says.
bool IsWired(const uint8_t* row, uint16_t* base, uint16_t* target);

// Gives a blank (or own) row the save shape and a tooltip line "+N% <string>". Returns false
// and writes nothing when the row is someone else's.
bool Shape(uint8_t* row, uint16_t stringId, uint16_t priority);

// Makes stat `slot` add its value in percent of `base` to `target`. The row must be blank or
// ours. Checks everything first; on false nothing was written and `problem` says why.
bool Wire(uint8_t* rows, size_t count, uint16_t slot, uint16_t base, uint16_t target, std::string& problem);
// Takes the property's entries out of the two other rows only. The row itself still names its
// target, so when its stat is touched on a unit the game works that target out again - now
// without the property. Do this, have the game recalculate, then Wire() anew or Unwire().
void Unlink(uint8_t* rows, size_t count, uint16_t slot);
// Unlink(), and the row stops being an op. It keeps its save shape (13 bits) and its text, so
// an item that still carries the stat stays readable; the stat just does nothing.
void Unwire(uint8_t* rows, size_t count, uint16_t slot);

// ANY stat's op, linked the way the game links it after reading the table: one entry per target
// stat with the row's own param. UnlinkOp() takes the row's entries out of every other row and
// leaves the row itself alone (so touching its stat still makes the game work its targets out
// again); ClearOp() also makes the row no op. LinkOp() checks first (dry = only check); on
// false nothing was written. It unlinks whatever the row was linked to before.
void UnlinkOp(uint8_t* rows, size_t count, uint16_t slot);
void ClearOp(uint8_t* rows, size_t count, uint16_t slot);
bool LinkOp(uint8_t* rows, size_t count, uint16_t slot, uint8_t op, uint8_t param, uint16_t base, const uint16_t targets[3], bool dry, std::string& problem);

// A Properties row "set stat `stat` to the rolled value" (func 1), as the game's `regen` row is.
void FillProperty(uint8_t* row, uint16_t id, uint16_t stat);

} // namespace prop
