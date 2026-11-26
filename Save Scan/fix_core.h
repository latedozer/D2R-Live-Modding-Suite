// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Save Scan: the save writer. When stats are removed, renumbered or get another saved shape, every
// saved item is rewritten to the table as it will be. No game, no Windows: it works on file bytes
// and plain tables (save_core.h) and reaches the disk only through FixIo, so test/fix_test.cpp
// runs it natively against copies of real saves.
//
// An item is rewritten bit for bit: everything before its first stat list and after its last one
// is copied as it is; each stat list is written again (the new id; param and value re-encoded with
// the shape after the change; a dropped stat left out). The item ends on the next whole byte as
// before. Then: .d2s size (byte 8) and checksum (byte 12); .d2i the length of every section.
// Nothing else in either file holds a length or a place (checked: every byte outside the items
// must be as before, apart from those fields).
#pragma once

#include "save_core.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace savescan {

struct ChangeSpec {
	uint16_t from;
	int32_t  to; // -1: dropped
};
struct ResizeSpec {
	uint16_t stat; // the id now
	uint8_t  saveBits, paramBits;
	int32_t  saveAdd;
};

// A saved affix id that changes (the number the item holds: magic 2 x 11 bits, rare/crafted up to
// 6 x (flag + 11 bits)). to = -1: dropped - a magic item's id becomes 0 ("none", as the saves have
// it on items with only a prefix or only a suffix), a rare/crafted item's slot flag becomes 0 (the
// item gets 11 bits shorter). The item keeps its stats: they are saved in its stat lists.
struct AffixSpec {
	uint8_t  suffix; // 0 prefix, 1 suffix
	uint16_t from;   // 1..2047
	int32_t  to;     // 1..2047, or -1
};

struct StatMap {
	std::map<uint16_t, int32_t> affixTo[2]; // [prefix, suffix]: id now -> id after (-1 dropped)
	int32_t affix(uint8_t suffix, uint16_t id) const {
		const auto found = affixTo[suffix & 1].find(id);
		return found == affixTo[suffix & 1].end() ? id : found->second;
	}
	std::vector<int32_t>   to;      // by id now: the id after, -1 dropped
	std::vector<StatShape> shape;   // by id now: its saved shape after
	std::vector<uint8_t>   resized; // by id now: 1 = its shape changes
	Tables                 after;   // the table after the change (by the new ids), item kinds as before
	bool touches(uint16_t id) const { return id < to.size() && (to[id] != id || resized[id] != 0); }
};
// The stats the game saves together, in its own order (17 with 18; 48, 50, 52 each with the next;
// 54 and 57 each with the next two): a change may not move or drop one of them.
bool InGroup(int id);
bool MakeMap(const Tables& before, const std::vector<ChangeSpec>& changes, const std::vector<ResizeSpec>& resizes, StatMap& map, std::string& problem, const std::vector<AffixSpec>& affixes = {});

uint32_t CharacterChecksum(const std::string& bytes);

// The shared stash's record section (type 2): bytes C0 ED EA C0, u16 1, u16 sets, u16 uniques,
// u16 runewords, 8 zero bytes, 10-byte entries, then 64 more bytes: the game's section writer
// (D2R.exe 0x530900) counts the 64-byte section header twice in the length, so 64 bytes of whatever
// was left in its buffer follow (zero, or stale item bytes). The game reads the entries by their
// counts and never those bytes: they are kept as they are (the read-back compare checks it).
// False only when the record does not have the known shape (why); note says what was kept.
bool StashRecordTailSafe(const std::string& bytes, std::string& why, std::string& note); // .d2s: rotate left, add each byte (bytes 12..15 as 0)

struct Rewrite {
	std::string       bytes;
	bool              changed = false;
	uint32_t          itemsChanged = 0, dropped = 0, renumbered = 0, resized = 0, affixesRenumbered = 0, affixesDropped = 0;
	std::vector<Item> expected; // the items as they must read with map.after
};
// `read` = this file read with `before`'s tables (ok). False: the change cannot be made to this file (why).
bool RewriteFile(bool stash, const std::string& bytes, const FileResult& read, const StatMap& map, Rewrite& out, std::string& problem);
// Every item as expected (all but its places in the file). False: the first difference in why.
bool SameItems(const std::vector<Item>& expected, const std::vector<Item>& got, std::string& why);
// Reads `bytes` (the rewritten file) with map.after and checks it against the original: every item
// as expected, the attributes as before, every byte outside the items as before (but the size,
// checksum and section-length fields), the .d2s size and checksum right.
bool CheckRewrite(bool stash, const std::string& original, const FileResult& read, const std::string& bytes, const StatMap& map, const Rewrite& rewrite, std::string& why);

// ---- the whole run over all save files

enum class FixState : int32_t { Unchanged = 0, Written = 1, WouldWrite = 2, Refused = 3, Restored = 4, Held = 5 }; // = savescan::FileState

struct FixInput {
	std::string   name;  // "Hellpal.d2s"
	bool          stash = false;
	std::string   bytes; // as read from disk
	const Tables* tables = nullptr; // the stat table (data version) to read it with
	int64_t       writeTime = 0;    // last written, unix seconds (for notAfter; 0 = not known)
};

// The saved shapes the files were written with, over the table's current ones (FixRequest.before).
// False: an id past 510 or a shape outside 1-32 save bits / 0-32 param bits (why).
bool ApplyBefore(Tables& tables, const std::vector<ResizeSpec>& before, std::string& why);

struct FixFileOut {
	std::string name;
	FixState    state = FixState::Unchanged;
	uint32_t    items = 0, itemsChanged = 0, dropped = 0, renumbered = 0, resized = 0, affixesRenumbered = 0, affixesDropped = 0;
	int32_t     bytesBefore = 0, bytesAfter = 0;
	std::string why;
	std::string note; // what was found and accepted (the stash record's tail)
};

struct FixOut {
	bool                    ok = false, dryRun = false;
	std::vector<FixFileOut> files;
	uint32_t                changed = 0, itemsChanged = 0, dropped = 0, renumbered = 0, resized = 0, affixesRenumbered = 0, affixesDropped = 0, unreadable = 0, restored = 0, notRestored = 0;
	std::string             backup, why;
};

// What a fix writes, kept in its backup before the first write: the file, its bytes before (the
// backup) and the size and hash of what is written over it. Restore puts a file back only when it
// still is what the fix wrote (a file that is already the backup counts as fine).
struct RestoreEntry {
	std::string name;
	std::string backup;            // the bytes before the fix
	uint64_t    writtenSize = 0, writtenHash = 0;
};
uint64_t Fnv64(const std::string& bytes); // FNV-1a, 64 bits

class FixIo {
public:
	virtual ~FixIo() = default;
	// May files be written now? (the plugin: no character in a game). Asked before the backup and
	// before every write. False: why.
	virtual bool mayWrite(std::string& why) {
		static_cast<void>(why);
		return true;
	}
	// The plan, kept in the backup folder before the first write. False: nothing is written.
	virtual bool planned(const std::vector<RestoreEntry>& plan, std::string& why) {
		static_cast<void>(plan);
		static_cast<void>(why);
		return true;
	}
	// Copies every file (all of them, changed or not) into a NEW dated backup folder and reads each
	// copy back; folder = its path. False: nothing may be written (why).
	virtual bool backup(const std::vector<FixInput>& files, std::string& folder, std::string& why) = 0;
	// Replaces a save file whole (written beside it, then moved into place).
	virtual bool write(const std::string& name, const std::string& bytes, std::string& why) = 0;
	virtual bool read(const std::string& name, std::string& bytes) = 0;
};

// dryRun: works everything out and checks it in memory, writes nothing.
// ok = false: no file on disk differs from what it was (or notRestored > 0, said in why).
// ok = true: every changed file was written, read back from disk and checked.
// Puts the files a fix wrote back from its backup (see RestoreEntry). ok = every file is as in the
// backup now; restored = files put back by this call; notRestored = files changed since the fix
// (left as they are, named in why).
FixOut Restore(const std::vector<RestoreEntry>& plan, FixIo& io);

// Stats Live Tables made (its grown-rows.txt: "itemstatcost<TAB>v=..<TAB>n=<number><TAB>col=value..."),
// with the saved shape their cells give. A row without save bits cannot be on a saved item.
struct MadeRow {
	uint16_t    number = 0;
	std::string name;
	StatShape   shape;
};
std::vector<MadeRow> MadeStatRows(const std::string& grownRows);
// The made rows the game's table lacks now (number past its rows), and the save files that hold
// one of them (read with the table plus those rows' shapes) - by name, for the warning.
std::vector<std::string> FilesHoldingMissing(const std::vector<FixInput>& files, const Tables& live, const std::vector<MadeRow>& rows, std::vector<MadeRow>& missing);

// notAfter (unix seconds, 0 = none): a file last written after it is refused (or of unknown time).
FixOut Fix(const std::vector<FixInput>& files, const std::vector<ChangeSpec>& changes, const std::vector<ResizeSpec>& resizes, bool dryRun, FixIo& io, const std::vector<AffixSpec>& affixes = {}, int64_t notAfter = 0);

} // namespace savescan
