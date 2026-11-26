// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties: finds a property code the game's own name table does not know yet - a property
// made on the Properties tab during this session (from the next start the game reads it under its
// code itself). This header is all a plugin needs; nothing is linked.
//
//   const liveprops::Api* props = liveprops::Find();   // nullptr: Live Properties is not installed
//   const int row = props != nullptr ? props->findProperty("my-prop", 3) : -1;
//
// Ask only when the game's own lookup does not know the code. Any thread, but not from inside one
// of Live Properties' own callbacks (Mod Book tabs, legend): it takes Live Properties' lock.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace liveprops {

constexpr uint32_t kApiVersion = 2; // 2 (2026-11-25): listMade

// One row Live Properties made (a stat in itemstatcost, a property in properties), as the game has
// it this start. Rows made during play that go live only at the next start are not listed.
struct MadeRow {
	char    name[64];    // stat name (itemstatcost "Stat") or property code (properties "code")
	int32_t number;      // its row number in the game's table this start
	int32_t dataVersion; // 1 classic, 2 LoD, 3 RotW
};
enum class Table : int32_t { stats = 0, properties = 1 };

struct Api {
	uint32_t size, version;
	// The property row of `code` in data version `dataVersion` (1 classic, 2 LoD, 3 RotW; 0 = the
	// newest that is on): its number in the game's property table, or -1 (no such code, or that data
	// version's tables are not read). Codes of the game's own rows are found too.
	int(__cdecl* findProperty)(const char* code, int dataVersion) noexcept;
	// Version 2. The rows Live Properties added to `table` in data version `dataVersion` (0 = the newest
	// that is on) this start, in row order: writes up to `room` into `out` (nullptr = count only) and
	// returns how many there are. Only rows the game really read in are listed.
	size_t(__cdecl* listMade)(Table table, int dataVersion, MadeRow* out, size_t room) noexcept;
};

inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-live-properties.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "LivePropsGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= offsetof(Api, findProperty) + sizeof(api->findProperty) ? api : nullptr; // callers check version >= 2 before listMade
}

} // namespace liveprops
