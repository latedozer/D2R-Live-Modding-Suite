// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Stand-in for d2rl-live-tables.dll (API 3): only the change listener and the remove veto.
// FakeTableChange calls the listeners as a growth / removal would, FakeAskRemove the veto as
// remove() does, both on the caller's thread.
#include "../../Live Tables/live_tables_api.h"

static livetables::ChangeFn g_listener = nullptr;
static void*                g_user     = nullptr;
static uint64_t             g_revision = 1;

static int __cdecl Check(livetables::Table, int, const char*, char*, size_t) noexcept { return 0; }
static int __cdecl Grow(livetables::Table, int, const char*, livetables::DoneFn, void*) noexcept { return 0; }
static int __cdecl Count(livetables::Table, int) noexcept { return -1; }
static int __cdecl Limit(livetables::Table, int) noexcept { return 32767; }
static int __cdecl Find(livetables::Table, int, const char*) noexcept { return -1; }
static uint64_t __cdecl Revision() noexcept { return g_revision; }
static int __cdecl AddListener(livetables::ChangeFn callback, void* user) noexcept {
	g_listener = callback;
	g_user     = user;
	return 1;
}
static void __cdecl RemoveListener(int) noexcept { g_listener = nullptr; }

static livetables::CanRemoveFn g_veto     = nullptr;
static void*                   g_vetoUser = nullptr;
static int __cdecl AddVeto(livetables::CanRemoveFn canRemove, void* user) noexcept {
	g_veto     = canRemove;
	g_vetoUser = user;
	return 1;
}
static void __cdecl RemoveVeto(int) noexcept { g_veto = nullptr; }

static const livetables::Api kApi { sizeof(livetables::Api), 3, Check, Grow, Count, Limit, Find, Revision, AddListener, RemoveListener,
	nullptr, nullptr, nullptr, nullptr, nullptr, AddVeto, RemoveVeto };

extern "C" __declspec(dllexport) const livetables::Api* __cdecl LiveTablesGetApi(uint32_t version) {
	return version >= 1 && version <= 3 ? &kApi : nullptr; // as 0.3.0: a version 1 caller reads the first fields
}

// the test: what the veto says to removing row `number` of table 0 (stats) / 1 (properties); -1 = no veto
extern "C" __declspec(dllexport) int __cdecl FakeAskRemove(int table, int dataVersion, int number, char* why, size_t whySize) {
	return g_veto != nullptr ? g_veto(g_vetoUser, static_cast<livetables::Table>(table), dataVersion, number, why, whySize) : -1;
}

// the test: kind 0 Grow, 4 Shrink, 5 Remove (of the Properties table, data version 3); 1 when a listener was called
extern "C" __declspec(dllexport) int __cdecl FakeTableChange(int kind, uint64_t number) {
	if (g_listener == nullptr) {
		return 0;
	}
	livetables::Change change {};
	change.size        = offsetof(livetables::Change, number); // API 1's Change
	change.kind        = static_cast<livetables::ChangeKind>(kind);
	change.table       = livetables::Table::Properties;
	change.dataVersion = 3;
	change.oldCount    = number;
	change.newCount    = number + 1;
	change.rowSize     = 0x30;
	change.revision    = ++g_revision;
	g_listener(g_user, &change);
	return 1;
}

// the test: row `number` of table 0 / 1 is renamed (Change kind Rename, API 3), data version 3
extern "C" __declspec(dllexport) int __cdecl FakeRename(int table, uint64_t number, const char* oldName, const char* newName) {
	if (g_listener == nullptr) {
		return 0;
	}
	livetables::Change change {};
	change.size        = sizeof(livetables::Change);
	change.kind        = livetables::ChangeKind::Rename;
	change.table       = static_cast<livetables::Table>(table);
	change.dataVersion = 3;
	change.oldCount = change.newCount = number + 1;
	change.rowSize     = table == 0 ? 0x144 : 0x30;
	change.revision    = ++g_revision;
	change.number      = number;
	change.oldName     = oldName;
	change.newName     = newName;
	g_listener(g_user, &change);
	return 1;
}
