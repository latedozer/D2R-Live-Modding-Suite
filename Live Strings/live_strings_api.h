// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// Live Strings (d2rl-live-strings.dll): text keys made while the game runs, through the loader's own
// string packages. A key added here can be named at once by a table cell (a stat's descstrpos /
// descstrneg / descstr2) and shows on the next tooltip draw. This header is all a plugin needs.
//
//   const livestrings::Api* ls = livestrings::Find();     // nullptr: Live Strings is not installed
//   livestrings::Text t {"stat391_pos", {"+%d%% of Life added to Mana"}};   // other languages = enUS
//   ls->add(&t, 1, OnDone, me);   // OnDone gets "live-strings:stat391_pos_r1" and its two-byte number
//   // then Live Tables' setCells writes descstrpos=live-strings:stat391_pos_r1 into the stat row.
//
// HOW (D2RCore, image 6ab3782c): every add() / setText() call writes ONE new string file
// data/local/lng/strings/d2rloader/live-strings/add-<n>.json and registers it with the loader's
// ResourceService. The loader then rebuilds its string tables at once (D2RCore 0x48bd60 -> 0x384780),
// keeping every two-byte number given out before. Nothing is ever unregistered: unregistering a file
// whose keys hold numbers makes D2RCore's rebuild fail and empties ALL game text. So:
//   - names are append-only; a name once added stays for the session (and is kept on disk);
//   - setText gives the name a NEW REVISION key (<name>_r2, _r3 ...) with the new text; the caller
//     writes the new key into its cells. Older revisions keep their text and stay defined.
// Every revision of every name is kept in d2rloader\live-suite\live-strings\<mod>\strings.json and
// registered at plugin load (before the tables are read), so cells kept by other plugins that name
// any revision find it at the next start.
// One call = one rebuild (a hitch, logged as "rebuild ... ms"): put every key of one action in one call.
//
// THREADS. add / setText / number / key / text / list / poolUse / status / listeners: any thread,
// not from inside a callback of this plugin. The files are registered in a UI job (in this install
// the UI thread is the game thread), done() and the listeners run there.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace livestrings {

constexpr uint32_t kApiVersion = 1;

// The 13 languages of the game's (and the mod's) string files, in this order. A missing translation
// does NOT fall back to English in D2RCore, so Live Strings writes every language: null = the enUS text.
constexpr int kLanguages = 13;
constexpr const char* kLanguageCodes[kLanguages] = {"enUS", "zhTW", "deDE", "esES", "frFR", "itIT", "koKR",
                                                   "plPL", "esMX", "jaJP", "ptBR", "ruRU", "zhCN"};

// A name and its text. name: 1-48 of A-Z a-z 0-9 _ (the key is "live-strings:<name>_r<revision>").
// lang[0] (enUS) is required (may be ""); others may be null. UTF-8, at most 4096 bytes each.
struct Text {
	const char* name;
	const char* lang[kLanguages];
};

struct KeyResult {
	const char* name;     // as asked
	const char* key;      // full key for table cells: "live-strings:<name>_r<revision>"
	int32_t     revision; // 1 for add, +1 per setText
	int32_t     number;   // the two-byte number D2RCore gave the key (0-65534), -1 if it gave none
};

// done(): exactly once per add() / setText() call that returned 1, on the UI thread.
// ok = 1: every key was registered (and kept on disk); ok = 0: nothing changed, why says it.
// Valid during the call only.
struct Done {
	uint32_t         size;    // sizeof(Done)
	int32_t          ok;
	uint32_t         count;
	const KeyResult* keys;    // count entries, in the order asked
	const char*      why;     // refusal, or "" / notes
	double           hitchMs; // the loader's rebuild time for this call
};
using DoneFn = void(__cdecl*)(void* user, const Done* done) noexcept;

// list(): one call per revision of every name, oldest first. latest = 1 for the revision a new cell
// should name. Valid during the call only.
struct Entry {
	uint32_t    size;     // sizeof(Entry)
	const char* name;
	const char* key;      // "live-strings:<name>_r<revision>"
	const char* localKey; // "<name>_r<revision>" (what an exported mod's own string file holds)
	int32_t     revision;
	int32_t     latest;
	const char* text[kLanguages];
};
using ListFn = void(__cdecl*)(void* user, const Entry* entry) noexcept;

enum class ChangeKind : int32_t { Added = 0, TextChanged = 1 };
struct Change {
	uint32_t    size;     // sizeof(Change)
	ChangeKind  kind;
	const char* name;
	const char* key;      // the new revision's full key
	int32_t     revision;
	int32_t     number;
};
// Called on the UI thread after a call's file is registered, once per key. Short, no locks, never
// call add / setText from it.
using ChangeFn = void(__cdecl*)(void* user, const Change* change) noexcept;

struct Api {
	uint32_t size, version;
	// Queue new names. Returns 1 (done will be called) or 0 (refused at once: why in done is not
	// called; status() says why Live Strings is off). A name already added is refused in done().
	int(__cdecl* add)(const Text* items, size_t count, DoneFn done, void* user) noexcept;
	// New text for names already added: each gets a new revision key.
	int(__cdecl* setText)(const Text* items, size_t count, DoneFn done, void* user) noexcept;
	// The latest revision's two-byte number (asks D2RCore, which gives one the first time), or -1.
	int(__cdecl* number)(const char* name) noexcept;
	// The latest revision's full key into out; returns its revision, or 0 when the name is unknown.
	int(__cdecl* key)(const char* name, char* out, size_t size) noexcept;
	// The latest text of one language (0-12) into out; returns 1, or 0 when unknown.
	int(__cdecl* text)(const char* name, int language, char* out, size_t size) noexcept;
	// Every revision (see Entry); returns how many.
	int(__cdecl* list)(ListFn callback, void* user) noexcept;
	// D2RCore's two-byte pool: numbers in use (game ids included) and the most there are (65535).
	// Returns 1, or 0 when it can't be read on this D2RCore.
	int(__cdecl* poolUse)(uint32_t* used, uint32_t* most) noexcept;
	// 1 = on; 0 = off, the reason in text.
	int(__cdecl* status)(char* text, size_t size) noexcept;
	int(__cdecl* addListener)(ChangeFn callback, void* user) noexcept; // handle > 0, or 0
	void(__cdecl* removeListener)(int handle) noexcept;
};

inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-live-strings.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "LiveStringsGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= offsetof(Api, removeListener) + sizeof(void*) ? api : nullptr;
}

} // namespace livestrings
