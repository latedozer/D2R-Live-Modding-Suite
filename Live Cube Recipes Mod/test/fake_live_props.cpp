// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Stand-in for d2rl-live-properties.dll: its lookup of a property made this session. It knows one
// code, "my-prop", at row 900 + the data version (so the test sees which version was asked), and
// whatever the test makes later with FakeMakeProperty (a property made while the game runs).
#include "../../Live Properties Mod/live_props_api.h"

#include <cstring>

static char g_late[64] = "";
static int  g_lateRow  = -1;

static int __cdecl FindProperty(const char* code, int dataVersion) noexcept {
	if (code != nullptr && strcmp(code, "my-prop") == 0) {
		return 900 + dataVersion;
	}
	return code != nullptr && g_late[0] != '\0' && strcmp(code, g_late) == 0 ? g_lateRow : -1;
}

static const liveprops::Api kApi { sizeof(liveprops::Api), liveprops::kApiVersion, FindProperty, nullptr };

extern "C" __declspec(dllexport) const liveprops::Api* __cdecl LivePropsGetApi(uint32_t version) {
	return version == liveprops::kApiVersion ? &kApi : nullptr;
}

// the test: `code` is (now) row `row` (-1: gone)
extern "C" __declspec(dllexport) void __cdecl FakeMakeProperty(const char* code, int row) {
	strncpy(g_late, code, sizeof(g_late) - 1);
	g_lateRow = row;
}
