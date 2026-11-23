// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Save Scan: asks "does any SAVED item hold this stat?" - every character file and the shared
// stash of the mod's save folder, read with the stat table the game holds in memory right now.
// This header is all a plugin needs; nothing is linked.
//
//   const savescan::Api* scan = savescan::Find();      // nullptr: Save Scan is not installed
//   savescan::Summary sum { sizeof(sum) };
//   const uint16_t stats[] = { 371 };
//   scan->scan(stats, 1, &sum, OnHit, OnProblem, user);
//   if (!savescan::Safe(sum)) { /* refuse the change; say why */ }
//
// The plugin does nothing until one of these is called (or `savescan` is typed in the console).
// A call reads the files there and then, on the calling thread (a few milliseconds per file),
// and reads the game's tables: call it from the game's thread or while no table is being
// loaded. Calls are taken one at a time. Saves are opened for reading only.
//
// Read the stats BEFORE changing their rows: the files hold items in the shape the table has
// at the time they were written.
#pragma once

#include <windows.h>

#include <cstdint>

namespace savescan {

constexpr uint32_t kApiVersion = 1;

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

struct Api {
	uint32_t size, version;
	// Looks for `count` stat numbers (count 0: only reads and counts). Returns summary->ran.
	// hit and problem may be nullptr.
	int(__cdecl* scan)(const uint16_t* stats, uint32_t count, Summary* summary, HitFn hit, ProblemFn problem, void* user) noexcept;
	// Writes d2rloader\save-scan\<mod>\items.txt: every item of every file with its stats, then
	// every stat in use with the properties that name it. path (may be nullptr) gets the file's
	// path as UTF-8. Returns 1 when the file was written.
	int(__cdecl* dump)(char* path, uint32_t pathSize, Summary* summary, ProblemFn problem, void* user) noexcept;
};

// Safe to change the saved shape of the stats asked for: the scan ran, every file was read to
// its end, and no item holds them.
inline bool Safe(const Summary& summary) {
	return summary.ran != 0 && summary.unreadable == 0 && summary.holders == 0;
}

// The Api of the Save Scan plugin loaded in this game, or nullptr.
inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-save-scan.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "SaveScanGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= sizeof(Api) ? api : nullptr;
}

} // namespace savescan
