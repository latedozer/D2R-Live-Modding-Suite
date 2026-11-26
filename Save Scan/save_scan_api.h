// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Save Scan: asks "does any SAVED item hold this stat?" - every character file and the shared
// stash of the mod's save folder, read with the stat table the game holds in memory right now -
// and (Api version 2) rewrites those files when a stat is removed, renumbered or resized.
// This header is all a plugin needs; nothing is linked.
//
//   const savescan::Api* scan = savescan::Find();      // nullptr: Save Scan is not installed
//   savescan::Summary sum { sizeof(sum) };
//   const uint16_t stats[] = { 371 };
//   scan->scan(stats, 1, &sum, OnHit, OnProblem, user);
//   if (!savescan::Safe(sum)) { /* refuse the change; say why */ }
//
// The plugin does nothing until one of these is called (or `savescan` is typed in the console).
// scan and dump read the files there and then, on the calling thread (a few milliseconds per
// file), and read the game's tables: call them from the game's thread or while no table is being
// loaded. Calls are taken one at a time. scan and dump open saves for reading only.
//
// Read the stats BEFORE changing their rows: the files hold items in the shape the table has
// at the time they were written.
//
// ---- fixSaves (version 2): the save writer
//
// For a change of the stat table that would leave saved items unreadable: a stat removed (its
// entries are dropped from every item, the item gets shorter), stats renumbered (a middle row
// removed: every later stat one down), a stat's saved shape changed (save bits / save add /
// save param bits: every saved value is re-encoded; refused if one does not fit).
//
//   const savescan::StatChange changes[] = { { 400, savescan::kDropped }, { 401, 400 }, { 402, 401 } };
//   savescan::FixRequest request { sizeof(request) };
//   request.changes = changes; request.changeCount = 3;
//   request.file = OnFile; request.done = OnDone; request.user = me;
//   scan->fixSaves(&request);            // returns at once; OnFile per file, then OnDone, on Save Scan's thread
//
// CALL IT BEFORE THE TABLE CHANGES, from the game thread or the UI thread (it takes a copy of the
// stat table's saved shapes there and then: a few microseconds, nothing else happens on the
// calling thread). All file work runs on Save Scan's own worker thread: nobody waits for it.
// file() and done() are called on that worker thread: keep them short, take no game lock, post
// what you need to your own next entry (done() is where the table change may be queued).
//
// What it does (nothing at all when one step fails before 4):
//   1. reads every .d2s and .d2i of the mod's save folder with the current table. Any file it
//      cannot read to its end: refused (what that file holds is not known);
//   2. works out every changed file in memory and reads it back from memory with the table as it
//      will be after the change; every item must be what it was, under the change (same items,
//      same order, same values; dropped stats gone, renumbered ids moved). Character attributes
//      ("gf") are read too: a change that would touch one of them is refused;
//   3. dry run (kDryRun): stops here, reports what WOULD change, writes nothing;
//   4. a new dated backup folder, d2rloader\live-suite\save-scan\<mod>\backup-YYYYMMDD-HHMMSS,
//      with every save file of the folder (read back and compared before going on);
//   5. writes each changed file (beside it, then moved into place), reads it back from disk,
//      compares every item again; on ANY difference or failed write, every file written is put
//      back from the backup and done() says so (ok = 0, restored = files put back).
//
// Before every write (the backup, each file) Save Scan checks again that no character is in a
// game; if one is, it stops and puts back what it wrote. Before the first write it keeps the plan
// (written.txt: each file, the size and hash of what is written) in the backup folder, so restore
// and the start-up check know what a fix did. A backup with that plan but no report.txt is a fix
// that was cut off: Save Scan says so at the main menu (log, console, its Mod Book tab "Saves")
// and offers to put it back.
//
// THE PROMISE: done.ok == 1 = every changed file was written, read back from disk and checked.
// done.ok == 0 = no file on disk differs from before (backups put back), unless notRestored > 0,
// which why then says in so many words (with the backup folder). done.why is a whole sentence
// that can be shown as it is.
//
// The shared stash's record of found items ends in 64 stale bytes (the game counts the section
// header twice): never read by the game, kept as they are (the note says so).
//
// Refused changes: stats the game saves together in its own order (17-18, 48-55, 57-59) cannot
// move or be dropped; a stat in a character's attributes ("gf": 0-15 in every table) cannot
// change; a new number past 510 or two stats on one number. "signed" does not change the saved
// encoding (the saves hold value + save add, unsigned), so it needs no fix.
//
// The loaded character and shared stash: the game holds them in memory and writes them itself,
// so they are never written while a character is in a game. For now (Api version 2) fixSaves
// REFUSES while a character is in a game (D2RCore's IsInGame), with that reason - a dry run is
// allowed in a game (it writes nothing; its findings include the loaded character): at the main
// menu nothing is loaded and every file is fixed on disk. (Live Tables researches fixing the
// loaded ones in memory; a later version will skip them on disk and report them as
// FileState::Held "fixed in memory by Live Tables".)
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace savescan {

constexpr uint32_t kApiVersion = 3; // SaveScanGetApi(v) gives this table for ANY v >= 1: check version and size

struct Hit { // one saved item that holds one of the stats asked for
	uint32_t    size;   // sizeof(Hit)
	const char* file;   // "Hellpal.d2s", "ModernSharedStashSoftCoreV2.d2i"
	const char* where;  // "worn", "inventory", "stash", "mercenary, worn", "stash tab 2", "in a socket" ...
	const char* code;   // the item's code: "cm1"
	uint16_t    stat;
	int32_t     value;  // as saved (save add taken off)
	uint32_t    param;
	uint32_t    offset; // the item's first byte in the file
};

struct Summary {
	uint32_t size;       // sizeof(Summary), set by the caller
	uint32_t ran;        // 1: the files were looked at. 0: the scan could not run at all (see the problem callback)
	uint32_t files;      // files found
	uint32_t unreadable; // files that could not be read to the end: what they hold is NOT known
	uint32_t items;      // items read
	uint32_t holders;    // items holding at least one of the stats asked for
};

// The strings are only good during the call.
using HitFn     = void(__cdecl*)(void* user, const Hit* hit) noexcept;
using ProblemFn = void(__cdecl*)(void* user, const char* file, const char* why) noexcept; // file "" = the scan as a whole

// ---- fixSaves

constexpr int32_t kDropped = -1;

// One stat id that changes. Ids not named keep their number and shape. A stat both renumbered
// and resized: one StatChange (from -> to) and one Resize (stat = from, the OLD id).
struct StatChange {
	uint16_t from; // the id now
	int32_t  to;   // the id after the change, or kDropped (its entries are removed from every item)
};

// A new saved shape for a stat (OLD id). Values are re-encoded: value = raw - old save add, raw'
// = value + new save add, which must fit in the new save bits (else refused); params must fit in
// the new param bits.
struct Resize {
	uint16_t stat;
	uint8_t  saveBits;  // 1..32
	uint8_t  paramBits; // 0..32
	int32_t  saveAdd;
};

// (0.3.3) A saved affix id that changes: the number the item holds (magic: prefix + suffix, 11 bits
// each, 0 = none; rare/crafted: up to 6, prefix/suffix by turns, each flag + 11 bits). to = kDropped:
// a magic item's id becomes 0 ("none"; refused if the item would have neither prefix nor suffix),
// a rare/crafted item's slot becomes empty (11 bits shorter). The item keeps its stats (they are
// saved in its stat lists); only the affix's name is gone from it.
struct AffixChange {
	uint8_t  suffix; // 0 prefix, 1 suffix
	uint16_t from;   // 1..2047
	int32_t  to;     // 1..2047, or kDropped
};

enum : uint32_t { kDryRun = 1 };

enum class FileState : int32_t {
	Unchanged  = 0, // nothing in it is touched by the change
	Written    = 1, // rewritten, read back from disk, every item compared: as expected
	WouldWrite = 2, // dry run: would be rewritten (worked out and checked in memory)
	Refused    = 3, // cannot be read, or the change cannot be done to it (why): nothing was written anywhere
	Restored   = 4, // was written, then put back from the backup (because of this file or another)
	Held       = 5, // (later versions) held by the game: left on disk, "fixed in memory by Live Tables"
};

struct FileReport { // file(): once per save file, in the order the files are worked on
	uint32_t    size;          // sizeof(FileReport)
	const char* file;          // "Hellpal.d2s"
	FileState   state;
	uint32_t    items;         // items in the file
	uint32_t    itemsChanged;  // items whose bytes change
	uint32_t    dropped;       // stat entries removed
	uint32_t    renumbered;    // stat entries given a new id
	uint32_t    resized;       // stat entries re-encoded
	int32_t     bytesBefore, bytesAfter;
	const char* why;           // Refused / Restored: the reason; else ""
	const char* note;          // (0.3.2, check size) what was found and kept, e.g. the stash record's tail; may be ""
	uint32_t    affixesRenumbered, affixesDropped; // (0.3.3, check size)
};

struct FixDone {
	uint32_t    size;          // sizeof(FixDone)
	int32_t     ok;            // 1: every file on disk now reads with the table after the change (dry run: would)
	int32_t     dryRun;
	uint32_t    files, changed, itemsChanged, dropped, renumbered, resized;
	uint32_t    unreadable;    // files that could not be read (then ok = 0, nothing written)
	uint32_t    restored;      // files put back from the backup (then ok = 0)
	uint32_t    notRestored;   // files that could NOT be put back (should never be > 0; then why names the backup to copy from by hand)
	const char* backup;        // the backup folder (UTF-8), "" when none was made
	const char* why;           // ok = 0: the reason; ok = 1: "" or notes
	uint32_t    affixesRenumbered, affixesDropped; // (0.3.3, check size)
};

using FileFn = void(__cdecl*)(void* user, const FileReport* report) noexcept;
using DoneFn = void(__cdecl*)(void* user, const FixDone* done) noexcept;

struct FixRequest {
	uint32_t          size;        // sizeof(FixRequest)
	uint32_t          flags;       // kDryRun
	const StatChange* changes;     // copied during the call
	uint32_t          changeCount;
	const Resize*     resizes;     // copied during the call
	uint32_t          resizeCount;
	FileFn            file;        // may be nullptr
	DoneFn            done;        // required
	void*             user;
	// (0.3.3) leave out (a shorter size) or 0 for none
	const AffixChange* affixChanges; // copied during the call
	uint32_t           affixChangeCount;
	// (0.3.4) The saved shape the files were WRITTEN with, for ids whose row now has another shape
	// or another meaning (a made stat that came back at another number after a mod update: before
	// {389: the shape of the made row}, change {389 -> 391}). Used for reading those ids instead of
	// the current row. Every id whose meaning differs between the saves and the table now must be
	// in `changes` (an id the saves' layout did not have: {id, kDropped} - no entry is expected).
	const Resize* before;          // stat = the id in the saves
	uint32_t      beforeCount;
	// (0.3.4) Unix time (seconds, UTC): a file last written after it is refused by name (written with
	// the table as it is now, it cannot be told apart). 0 = no check.
	int64_t       notAfter;
};

// (0.3.3) scanAffixes: the saved items that carry an affix id.
struct AffixId {
	uint8_t  suffix; // 0 prefix, 1 suffix
	uint16_t id;
};
struct AffixHit {
	uint32_t    size;   // sizeof(AffixHit)
	const char* file;
	const char* where;
	const char* code;
	uint8_t     suffix;
	uint8_t     slot;    // magic: 0 prefix, 1 suffix; rare/crafted: 0..5
	uint8_t     quality; // 4 magic, 6 rare, 8 crafted
	uint16_t    id;
	uint32_t    offset;  // the item's first byte in the file
};
using AffixHitFn = void(__cdecl*)(void* user, const AffixHit* hit) noexcept;

struct Api {
	uint32_t size, version;
	// Looks for `count` stat numbers (count 0: only reads and counts). Returns summary->ran.
	// hit and problem may be nullptr.
	int(__cdecl* scan)(const uint16_t* stats, uint32_t count, Summary* summary, HitFn hit, ProblemFn problem, void* user) noexcept;
	// Writes d2rloader\live-suite\save-scan\<mod>\items.txt: every item of every file with its
	// stats, then every stat in use with the properties that name it. path (may be nullptr) gets
	// the file's path as UTF-8. Returns 1 when the file was written.
	int(__cdecl* dump)(char* path, uint32_t pathSize, Summary* summary, ProblemFn problem, void* user) noexcept;
	// --- version 2 (check api->version >= 2)
	// Starts the save writer (see above). Returns 1: done() WILL be called exactly once (on Save
	// Scan's thread; also for a refusal: in a game, busy with another fix, a bad change list).
	// 0: request or done is nullptr / size too small: nothing started, no callback.
	int(__cdecl* fixSaves)(const FixRequest* request) noexcept;
	// --- 0.3.3 (check api->size >= offsetof(Api, scanAffixes) + sizeof(void*))
	// Like scan, for affix ids (same thread rules). summary->holders = items carrying one of them.
	int(__cdecl* scanAffixes)(const AffixId* ids, uint32_t count, Summary* summary, AffixHitFn hit, ProblemFn problem, void* user) noexcept;
	// --- version 3 (0.3.4)
	// fixSaves on the CALLER's thread: the same steps and promise; file() and done() (both may be
	// nullptr here) are called on the caller's thread during it; `out` is filled (out->size set by the
	// caller), its strings valid until the next fixSavesNow / restore call. 1 = ran (read out->ok);
	// 0 = bad arguments. While an asynchronous fixSaves runs: 1 with ok = 0, "busy".
	int(__cdecl* fixSavesNow)(const FixRequest* request, FixDone* out) noexcept;
	// Puts the files a fix wrote back from its backup (backupFolder = done.backup exactly; only
	// Save Scan's own backups of the current save folder are taken). Caller's thread. Each file
	// only when it still is what the fix wrote; a file that is already the backup counts as fine
	// (safe to call twice); a file changed since (the game saved it?) is left as it is (notRestored,
	// named in why). Every file put back is read back and compared. Refused in a game.
	// 1 = ran (out->ok = every file the fix wrote is as in the backup; out->restored = put back now).
	int(__cdecl* restore)(const char* backupFolder, FixDone* out) noexcept;
};

// Safe to change the saved shape of the stats asked for: the scan ran, every file was read to
// its end, and no item holds them.
inline bool Safe(const Summary& summary) {
	return summary.ran != 0 && summary.unreadable == 0 && summary.holders == 0;
}

// The Api of the Save Scan plugin loaded in this game, or nullptr. minVersion 2 for fixSaves,
// 3 for fixSavesNow / restore. (A newer Save Scan gives its newer table to every caller.)
inline const Api* Find(uint32_t minVersion = 1) {
	const HMODULE module = GetModuleHandleW(L"d2rl-save-scan.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "SaveScanGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	if (api == nullptr && get != nullptr && minVersion <= 1) {
		api = get(1);
	}
	const size_t need = minVersion >= 3 ? offsetof(Api, restore) + sizeof(void*) : minVersion >= 2 ? offsetof(Api, scanAffixes) : offsetof(Api, fixSaves);
	return api != nullptr && api->version >= minVersion && api->size >= need ? api : nullptr;
}

} // namespace savescan
