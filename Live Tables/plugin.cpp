// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Tables: the game's ItemStatCost and Properties grow by a row while the game runs, in the
// game's memory, with the game's own routines (live_tables_api.h says how). No table reload, no
// restart, the mod's own files are never written.
//
// Rows grown are kept in d2rloader\live-suite\live-tables\<mod>\grown-rows.txt and laid into the
// table's text every time the game reads it (the only hook on D2RCore's read routine 0x29a560).
//
// Console (Ctrl + `): tables | tables grow stat <column=value;...> | tables grow prop <column=value;...>

#include <D2RLPlugin/api.h>

#include "live_tables_api.h"
#include "name_list.h"
#include "../Save Scan/save_scan_api.h"
#include "table_model.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.3.4";

const D2RL::PluginContext*  g_ctx     = nullptr;
const D2RL::ThreadService*  g_threads = nullptr;
const D2RL::ItemService*    g_items   = nullptr; // the loaded character's items (remove: who holds a stat)
const D2RL::InventoryService* g_inventory = nullptr;
bool                        g_enabled = true;

void LogF(const char* format, ...) {
	if (g_ctx == nullptr) return;
	char    text[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

void LoadConfig() {
	static char text[2048];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) return;
	for (const char* line = text; line != nullptr && *line != '\0';) {
		const char* end = strchr(line, '\n');
		char buffer[256], key[32], value[32];
		snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(end != nullptr ? end - line : static_cast<ptrdiff_t>(strlen(line))), line);
		line = end != nullptr ? end + 1 : nullptr;
		if (sscanf(buffer, " %31[a-z_] = %31s", key, value) == 2 && strcmp(key, "enabled") == 0) g_enabled = strncmp(value, "true", 4) == 0 || *value == '1';
	}
}

// ---------------------------------------------------------------- the game

constexpr uint32_t  kKnownStamp = 0x6ab3782c, kKnownCoreStamp = 0x6ab37824;
constexpr uintptr_t kTablesRva  = 0x300a90;  // data tables of a data version (1 classic, 2 LoD, 3 RotW)
constexpr uintptr_t kLookupRva  = 0xa121f0;  // name table lookup (table, text, complain) -> number or -1
constexpr uintptr_t kInsertRva  = 0xa11e10;  // name table insert (table, text, keepExisting) -> number
constexpr uintptr_t kStringRva  = 0x308be0;  // string key -> u16 number (unknown key -> 0x1506)
constexpr uintptr_t kAllocRva   = 0x12063c0; // the game's allocator: vtable +8 alloc(this, size, align), +0x20 free(this, p)
constexpr uintptr_t kStatsAt = 0x1258, kPropsAt = 0x240; // {begin, count, capacity}
constexpr uintptr_t kNameAt[3] = { 0x1270, 0x270, 0x1d8 }; // stat names, property codes, events (pointers)
constexpr uintptr_t kNameNextAt = 0x38;      // in a name table: the next number
constexpr uintptr_t kReadTextRva = 0x29a560, kClearReserveRva = 0x1d1400, kWideFlagRva = 0x7168d8; // D2RCore

struct Print { uintptr_t rva; const char* what; uint8_t size; uint8_t bytes[16]; };
constexpr Print kPrints[] = {
	{ kTablesRva, "the data tables getter", 16, { 0x48, 0x83, 0xec, 0x28, 0x0f, 0xb6, 0xc1, 0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0x83, 0xf8, 0x04 } },
	{ kLookupRva, "the name table lookup", 16, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x05 } },
	{ kInsertRva, "the name table insert", 16, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8b } },
	{ kStringRva, "the string key routine", 16, { 0x48, 0x83, 0xec, 0x38, 0x80, 0x39, 0x00, 0x48, 0x89, 0x4c, 0x24, 0x20, 0x74, 0x12, 0x48, 0xc7 } },
	{ kAllocRva, "the allocator getter", 13, { 0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 } },
	{ 0x31ecd5, "the stat table's place", 7, { 0x48, 0x8d, 0xbe, 0x58, 0x12, 0x00, 0x00 } },
	{ 0x31ed76, "the stat row size", 9, { 0x48, 0x69, 0x44, 0x24, 0x50, 0x44, 0x01, 0x00, 0x00 } },
	{ 0x31effb, "the op list's place", 7, { 0x66, 0x89, 0xb2, 0xdc, 0x00, 0x00, 0x00 } },
	{ 0x3d326d, "the property table's place", 7, { 0x48, 0x81, 0xc7, 0x40, 0x02, 0x00, 0x00 } },
	{ 0x3e5dd0, "the stat name table's place", 7, { 0x48, 0x8b, 0x88, 0x70, 0x12, 0x00, 0x00 } },
	{ 0x32042c, "the property code table's place", 7, { 0x48, 0x8b, 0x88, 0x70, 0x02, 0x00, 0x00 } },
	{ 0x1d996e0, "the op flag values", 16, { 0x10, 0, 0, 0, 0x20, 0, 0, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0 } },
	{ 0x4925cb, "the server's item buckets (game+0x2E30)", 16, { 0x41, 0x8b, 0xc0, 0x83, 0xe0, 0x7f, 0x48, 0x05, 0xc6, 0x05, 0x00, 0x00, 0x48, 0x8d, 0x04, 0xc2 } },
	{ 0x48fb79, "the unit chain (+0x158)", 7, { 0x48, 0x8b, 0x89, 0x58, 0x01, 0x00, 0x00 } },
};

HMODULE g_core = nullptr; // D2RCore.dll
// D2RCore's own stat-name lists (name_list.h): what the character environment digest names stats by
constexpr uintptr_t kCoreNewRva = 0x5136b8, kCoreDeleteRva = 0x513710, kCoreNamesRva = 0x7de2e0, kCoreNamesLockRva = 0x7de340;
constexpr Print kCorePrints[] = { // D2RCore code whose bytes (and rip-relative targets) name these addresses
	{ kCoreNewRva, "D2RCore's operator new", 9, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 } },
	{ kCoreDeleteRva, "D2RCore's operator delete", 5, { 0xe9, 0xf3, 0xff, 0xff, 0xff } },
	{ 0x3b745f, "the stat-name lists' lock (writer)", 7, { 0x48, 0x8d, 0x0d, 0xda, 0x6e, 0x42, 0x00 } },
	{ 0x3b7476, "the stat-name lists (writer)", 7, { 0x48, 0x8d, 0x0d, 0x63, 0x6e, 0x42, 0x00 } },
	{ 0x3b74b5, "the stat-name lists' lock (writer, release)", 7, { 0x48, 0x8d, 0x0d, 0x84, 0x6e, 0x42, 0x00 } },
	{ 0x3bc9c5, "the stat-name lists' lock (digest)", 7, { 0x48, 0x8d, 0x0d, 0x74, 0x19, 0x42, 0x00 } },
	{ 0x3bc9d5, "the stat-name lists (digest)", 7, { 0x48, 0x8d, 0x0d, 0x04, 0x19, 0x42, 0x00 } },
};
constexpr Print kThreadPrints[] = { // where D2RCore records the two threads (mov [rip+...], eax after GetCurrentThreadId)
	{ 0x837662, "the UI thread record (UpdateWidget)", 12, { 0xff, 0x15, 0xe8, 0x8b, 0xe1, 0xff, 0x89, 0x05, 0x12, 0x7a, 0xfa, 0xff } },
	{ 0x493825, "the game thread record (UpdateClientsAndAutosave)", 12, { 0xff, 0x15, 0x25, 0xca, 0x1b, 0x00, 0x89, 0x05, 0x53, 0xb8, 0x34, 0x00 } },
};
bool g_threadsKnown = false; // the prints above hold
// The unit walk (remove of a last row in a game: every item unit, server and client): D2RCore's
// session pointer [0x700480] + 0x2e0 (the check runOnGameThread makes, 0x493831 / 0x49383d) is
// taken for the server's game; its item buckets at +0x2E30 (128 x 8 bytes, 0x4925cb), chained by
// unit +0x158 (0x48fb79); the client's at D2R 0x2A23910 + 4 * 0x400.
constexpr Print kWalkPrints[] = {
	{ 0x493831, "D2RCore's session pointer read", 7, { 0x48, 0x8b, 0x05, 0x48, 0xcc, 0x26, 0x00 } },
	{ 0x49383d, "D2RCore's session game read", 7, { 0x48, 0x8b, 0x80, 0xe0, 0x02, 0x00, 0x00 } },
};
// The loader's own record of the UI thread (D2RCore writes GetCurrentThreadId() there in the root
// widget's update before it runs the UI jobs, UpdateWidget 0x837662) and of the game thread
// (UpdateClientsAndAutosave 0x493825, before it runs the game jobs).
constexpr uintptr_t kUiThreadIdRva = 0x7df080, kGameThreadIdRva = 0x7df084;
std::string g_namesProblem; // set: stats are not grown (the digest would name the new stat "")
void* (*g_coreNew)(size_t) = nullptr;
void (*g_coreDelete)(void*) = nullptr;
void* CoreNew(size_t n) { return g_coreNew(n); }
void  CoreDelete(void* p) { g_coreDelete(p); }
const ltn::Heap kCoreHeap { CoreNew, CoreDelete };
ltn::MsvcVector* CoreNames(int version) { return reinterpret_cast<ltn::MsvcVector*>(reinterpret_cast<uint8_t*>(g_core) + kCoreNamesRva + static_cast<uintptr_t>(version) * 0x18); }
PSRWLOCK CoreNamesLock() { return reinterpret_cast<PSRWLOCK>(reinterpret_cast<uint8_t*>(g_core) + kCoreNamesLockRva); }

struct Vector { uint8_t* rows; uint64_t count; int64_t capacity; };

struct Link {
	bool ok = false;
	uint8_t* (*tables)(uint8_t version)                            = nullptr;
	int32_t (*lookup)(void* table, const char* text, int complain) = nullptr;
	int32_t (*insert)(void* table, const char* text, bool keep)    = nullptr;
	uint16_t (*stringId)(const char* key)                          = nullptr;
	void* (*allocator)()                                           = nullptr;
	void (*descOrder)(void* tables)                                = nullptr; // D2RCore BuildItemStatDescriptionOrder
	const uint8_t* wideFlag = nullptr;
};
Link        g_link;
std::string g_linkProblem;
uintptr_t   g_imageBase = 0;

uint32_t ImageStamp(uintptr_t base) {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (base == 0 || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xE00) return 0;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

bool Readable(const void* address, size_t size) {
	MEMORY_BASIC_INFORMATION info {};
	if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
	return static_cast<const uint8_t*>(address) + size <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

bool OneObject12() { return g_link.tables(1) == g_link.tables(2); }

uint16_t (*g_stringIdOr)(const void* keyView, uint16_t fallback) = nullptr; // D2RCore GetNamespacedStringIdOrDefault({text, length}, default)
bool LinkGame() {
	g_imageBase = g_ctx->exeBase != 0 ? g_ctx->exeBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	char text[256];
	if (const uint32_t stamp = ImageStamp(g_imageBase); stamp != kKnownStamp) {
		snprintf(text, sizeof(text), "this is another build of the game (image stamp %08x; Live Tables knows %08x)", stamp, kKnownStamp);
		g_linkProblem = text;
		return false;
	}
	if (g_core == nullptr) g_core = GetModuleHandleW(L"D2RCore.dll");
	if (g_core == nullptr) { g_linkProblem = "D2RCore.dll is not loaded"; return false; }
	if (const uint32_t stamp = ImageStamp(reinterpret_cast<uintptr_t>(g_core)); stamp != kKnownCoreStamp) {
		snprintf(text, sizeof(text), "this is another D2RCore.dll (stamp %08x; Live Tables knows %08x)", stamp, kKnownCoreStamp);
		g_linkProblem = text;
		return false;
	}
	g_link.tables    = reinterpret_cast<decltype(g_link.tables)>(g_imageBase + kTablesRva);
	g_link.lookup    = reinterpret_cast<decltype(g_link.lookup)>(g_imageBase + kLookupRva);
	g_link.insert    = reinterpret_cast<decltype(g_link.insert)>(g_imageBase + kInsertRva);
	g_link.stringId  = reinterpret_cast<decltype(g_link.stringId)>(g_imageBase + kStringRva);
	g_link.allocator = reinterpret_cast<decltype(g_link.allocator)>(g_imageBase + kAllocRva);
	g_link.descOrder = reinterpret_cast<decltype(g_link.descOrder)>(reinterpret_cast<void*>(GetProcAddress(g_core, "BuildItemStatDescriptionOrder")));
	g_link.wideFlag  = reinterpret_cast<const uint8_t*>(g_core) + kWideFlagRva;
	g_stringIdOr     = reinterpret_cast<decltype(g_stringIdOr)>(reinterpret_cast<void*>(GetProcAddress(g_core, "GetNamespacedStringIdOrDefault")));
	if (g_link.descOrder == nullptr) { g_linkProblem = "D2RCore.dll has no BuildItemStatDescriptionOrder"; return false; }
	for (const Print& print : kPrints) {
		const auto* at = reinterpret_cast<const uint8_t*>(g_imageBase + print.rva);
		if (memcmp(at, print.bytes, print.size) != 0) {
			if (at[0] == 0xe9 || (at[0] == 0xff && at[1] == 0x25)) {
				g_linkProblem = std::string(print.what) + " starts with a jump (another plugin hooked it)";
			} else {
				g_linkProblem = std::string("the game's code is not what Live Tables expects at ") + print.what;
			}
			return false;
		}
	}
	for (const Print& print : kCorePrints) {
		const auto* at = reinterpret_cast<const uint8_t*>(g_core) + print.rva;
		if (!Readable(at, print.size) || memcmp(at, print.bytes, print.size) != 0) {
			g_namesProblem = std::string("D2RCore's code is not what Live Tables expects at ") + print.what;
			break;
		}
	}
	g_threadsKnown = true;
	for (const Print& print : kThreadPrints) {
		const auto* at = reinterpret_cast<const uint8_t*>(g_core) + print.rva;
		if (!Readable(at, print.size) || memcmp(at, print.bytes, print.size) != 0) g_threadsKnown = false;
	}
	g_coreNew    = reinterpret_cast<void* (*)(size_t)>(reinterpret_cast<uint8_t*>(g_core) + kCoreNewRva);
	g_coreDelete = reinterpret_cast<void (*)(void*)>(reinterpret_cast<uint8_t*>(g_core) + kCoreDeleteRva);
	g_link.ok = true;
	return true;
}

// the game allocator: vtable +8 alloc(this, size, align) (as 0xa0f682), +0x20 free(this, p) (as 0x2fcbcc)
void* GameAlloc(size_t size) {
	void* a = g_link.allocator();
	auto  f = reinterpret_cast<void* (*)(void*, size_t, size_t)>((*reinterpret_cast<void***>(a))[1]);
	return f(a, size, 0x10);
}
void GameFree(void* p) {
	void* a = g_link.allocator();
	auto  f = reinterpret_cast<void (*)(void*, void*)>((*reinterpret_cast<void***>(a))[4]);
	f(a, p);
}

// ---------------------------------------------------------------- files

std::wstring Widen(const std::string& text) {
	std::wstring out(text.size(), L'\0');
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
	out.resize(n > 0 ? static_cast<size_t>(n) : 0);
	return out;
}

std::wstring g_loaderDir, g_folder;

std::wstring LoaderDir() {
	std::wstring loader;
	if (g_ctx->pluginDirectory != nullptr && g_ctx->pluginDirectory[0] != L'\0') {
		loader = g_ctx->pluginDirectory;
		while (!loader.empty() && (loader.back() == L'\\' || loader.back() == L'/')) loader.pop_back();
		const size_t slash = loader.find_last_of(L"\\/");
		loader = slash != std::wstring::npos && _wcsicmp(loader.c_str() + slash + 1, L"plugins") == 0 ? loader.substr(0, slash) : std::wstring();
	}
	if (loader.empty()) {
		wchar_t path[1024] = {};
		const DWORD length = GetModuleFileNameW(nullptr, path, 1000);
		loader.assign(path, length < 1000 ? length : 0);
		const size_t slash = loader.find_last_of(L'\\');
		loader = (slash != std::wstring::npos ? loader.substr(0, slash) : loader) + L"\\d2rloader";
	}
	return loader;
}

// d2rloader\live-suite\live-tables\<mod>: beside the plugins folder, never inside a mod's .mpq
std::wstring OwnFolder() {
	if (!g_folder.empty()) return g_folder;
	g_loaderDir = LoaderDir();
	const std::string mod = g_ctx->activeMod != nullptr && g_ctx->activeMod[0] != '\0' ? g_ctx->activeMod : "no mod";
	g_folder = g_loaderDir + L"\\live-suite\\live-tables\\" + Widen(mod);
	return g_folder;
}

void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	CreateDirectoryW(path.c_str(), nullptr);
}

bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	text.clear();
	char buffer[4096];
	DWORD got = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got != 0) text.append(buffer, got);
	CloseHandle(file);
	return true;
}

bool WriteOwn(const std::wstring& path, const std::string& text) { // a new file, then moved over the old one
	MakeFolders(path.substr(0, path.find_last_of(L'\\')));
	const std::wstring temp = path + L".new";
	const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD put = 0;
	const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
	CloseHandle(file);
	return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring KeptPath() { return OwnFolder() + L"\\grown-rows.txt"; }
constexpr char kKeptHeader[] =
	"# Live Tables: rows grown while the game ran. Each is added again every time the game reads the table.\n"
	"# table<TAB>v=<data versions><TAB>n=<number when made><TAB>column=value...  (close the game before editing)\n";

// Does a DLL file export `name`? (read from the file: the DLL is not loaded for this)
bool FileExports(const std::wstring& path, const char* name) {
	std::string d;
	if (!ReadWhole(path, d) || d.size() < 0x400) return false;
	auto u32 = [&](size_t o) -> uint32_t { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; };
	auto u16 = [&](size_t o) -> uint16_t { uint16_t v = 0; if (o + 2 <= d.size()) memcpy(&v, d.data() + o, 2); return v; };
	const uint32_t pe = u32(0x3c);
	if (pe + 0x108 > d.size() || u32(pe) != 0x4550) return false;
	const uint16_t sections = u16(pe + 6), optSize = u16(pe + 20);
	const size_t   opt = pe + 24;
	const uint32_t exportRva = u32(opt + 112);
	auto off = [&](uint32_t rva) -> size_t {
		for (uint16_t i = 0; i < sections; ++i) {
			const size_t s = opt + optSize + i * 40u;
			const uint32_t vs = u32(s + 8), va = u32(s + 12), rs = u32(s + 16), ro = u32(s + 20);
			if (rva >= va && rva < va + (vs > rs ? vs : rs)) return rva - va + ro;
		}
		return 0;
	};
	const size_t e = off(exportRva);
	if (exportRva == 0 || e == 0) return false;
	const uint32_t names = u32(e + 24), table = u32(e + 32);
	for (uint32_t i = 0; i < names; ++i) {
		const size_t n = off(u32(off(table) + i * 4u));
		if (n != 0 && n < d.size() && strcmp(d.c_str() + n, name) == 0) return true;
	}
	return false;
}

// The sha256 of this DLL's file (logged at start: which build runs).
std::string OwnSha256() {
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnSha256), &self)) return "?";
	wchar_t path[1024] = {};
	if (GetModuleFileNameW(self, path, 1000) == 0) return "?";
	std::string data;
	if (!ReadWhole(path, data)) return "?";
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	uint8_t digest[32] = {};
	std::string hex = "?";
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
		if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
			if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0 && BCryptFinishHash(hash, digest, 32, 0) == 0) {
				hex.clear();
				char b[3];
				for (uint8_t c : digest) { snprintf(b, sizeof(b), "%02x", c); hex += b; }
			}
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	return hex;
}

// ---------------------------------------------------------------- state

SRWLOCK g_lock = SRWLOCK_INIT; // everything below except the listeners' list
struct Text {                  // a table's text as the game read it (+ the rows grown into it)
	bool                   seen = false;
	std::vector<lt::Sheet> reads; // every read of the file this start (D2RCore reads some twice: a
	                              // base snapshot and the source); the one whose rows match is used
	lt::Sheet* Matching(size_t count) {
		for (auto it = reads.rbegin(); it != reads.rend(); ++it) if (it->rows.size() == count) return &*it;
		return nullptr;
	}
};
Text g_texts[2][2];            // [table][0 = excel (data version 3), 1 = excel\base (1 and 2)]
// Kept rows whose number at this start is not the one they had when made (a mod update below them,
// or an exported mod holding them elsewhere). A STAT so moved: saved items read it as another stat, so
// every ItemStatCost change is refused and an alarm is raised (status(), Change kind Alarm) until
// the saves are fixed at the menu. A property so moved touches no save: its kept number follows.
std::vector<lt::Moved> g_moved; // under g_lock; cleared when the tables are read again
std::string g_alarm;            // the alarm text ("" = none)
std::vector<lt::Moved> g_movedStats; // the stats of the alarm (for tables fixmoved)
std::vector<lt::Moved> g_movedLast;  // every moved row of the last table read (moved())
int64_t g_startTime = 0;        // unix time of this table read: saves written later cannot be told apart
std::vector<lt::KeptRow> g_kept;
bool g_keptRead = false;
struct Retired { uint8_t* rows; bool gameOwned; };
std::vector<Retired> g_retired; // old buffers, freed at the next table load
bool        g_tableHook = false;
std::string g_hookProblem;
std::atomic<uint64_t> g_revision { 1 };

int  TextIndex(int version) { return version == 3 ? 0 : 1; }
bool BankUp(int version) {
	if (version < 1 || version > 3) return false;
	uint8_t* t = g_link.tables(static_cast<uint8_t>(version));
	return t != nullptr && Readable(t, kStatsAt + sizeof(Vector)) && reinterpret_cast<Vector*>(t + kStatsAt)->rows != nullptr;
}
Vector* VectorOf(lt::TableId table, int version) {
	return reinterpret_cast<Vector*>(g_link.tables(static_cast<uint8_t>(version)) + (table == lt::TableId::ItemStatCost ? kStatsAt : kPropsAt));
}
void* NamesOf(int version, int names) { return *reinterpret_cast<void**>(g_link.tables(static_cast<uint8_t>(version)) + kNameAt[names]); }
size_t LimitOf(lt::TableId table) {
	return table == lt::TableId::ItemStatCost ? (*g_link.wideFlag == 1 ? lt::kMaxStatsGame : lt::kMaxStatsRetail) : lt::kMaxProperties;
}
lt::Resolver GameResolver(int version) {
	lt::Resolver r;
	r.find     = [version](int names, const std::string& s) { void* t = NamesOf(version, names); return t != nullptr ? g_link.lookup(t, s.c_str(), 0) : -1; };
	r.insert   = [version](int names, const std::string& s) { void* t = NamesOf(version, names); return t != nullptr ? g_link.insert(t, s.c_str(), true) : -1; };
	r.stringId = [](const std::string& key) { return g_link.stringId(key.c_str()); };
	return r;
}

bool g_keptFileRead = false; // grown-rows.txt was there and read (under g_lock)
void ReadKeptOnce() { // under g_lock
	if (g_keptRead) return;
	g_keptRead = true;
	std::string file;
	if (!ReadWhole(KeptPath(), file)) return;
	g_keptFileRead = true;
	std::vector<std::string> problems;
	g_kept = lt::ReadKept(file, &problems);
	for (const std::string& p : problems) LogF("grown-rows.txt: %s (left out)", p.c_str());
}

// ---------------------------------------------------------------- the read hook

struct ReadResult {            // as D2RCore's read routine fills it
	uint8_t  path[32];         // std::string: text or pointer +0, size +0x10, capacity +0x18
	int32_t  status;
	uint32_t pad;
	uint8_t* begin;            // +0x28 std::vector<char> of the file's bytes
	uint8_t* end;
	uint8_t* capacity;
	uint8_t  ok;               // +0x40
};
static_assert(offsetof(ReadResult, begin) == 0x28 && offsetof(ReadResult, ok) == 0x40, "D2RCore's read result");
// d2rcore 0x29a560: 55 41 57 41 56 41 54 56 57 53 48 81 ec d0 00 00 00
constexpr uint8_t kReadTextStart[17] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00 };
// d2rcore 0x1d1400: 56 57 48 83 ec 28 48 85 d2
constexpr uint8_t kClearReserveStart[9] = { 0x56, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xd2 };
using ReadTextFn     = uint64_t (*)(uint64_t flag, const char* name, const char* ext, ReadResult* out);
using ClearReserveFn = void (*)(void* vector, uint64_t count);
ReadTextFn     g_readText     = nullptr;
ClearReserveFn g_clearReserve = nullptr;

// "data/global/excel/itemstatcost.txt" (data version 3) or ".../excel/base/..." (1 and 2)
bool WhichTable(std::string path, lt::TableId& table, int& text) {
	for (char& c : path) c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
	const auto ends = [&](const char* tail) { const size_t n = strlen(tail); return path.size() >= n && path.compare(path.size() - n, n, tail) == 0; };
	for (int t = 0; t < 2; ++t) {
		const std::string file = std::string(lt::TableFileName(static_cast<lt::TableId>(t))) + ".txt";
		if (ends(("excel/base/" + file).c_str())) { table = static_cast<lt::TableId>(t); text = 1; return true; }
		if (ends(("excel/" + file).c_str()))      { table = static_cast<lt::TableId>(t); text = 0; return true; }
	}
	return false;
}

// Another table's text: which kept rows does it name? A cell equal to the name (skills' aurastat,
// states' stat, items' stat1-3, ...) or the name quoted inside a formula (calc columns:
// stat('name'.accr)). Those tables hold the row's number once compiled, and Live Tables does not
// rewrite them: such a row - and, for a middle row, any row after it - is not removed.
std::map<std::string, std::set<std::string>> g_namedBy; // lower-case name -> "path, row k"; under g_lock
void NoteNames(const std::string& path, const char* text, size_t size) {
	std::string p = path;
	for (char& c : p) c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
	if (p.find("excel/") == std::string::npos) return;
	AcquireSRWLockExclusive(&g_lock);
	ReadKeptOnce();
	for (auto& [name, places] : g_namedBy) // read again: what it names now
		for (auto it = places.begin(); it != places.end();) it = it->compare(0, p.size() + 1, p + ",") == 0 ? places.erase(it) : std::next(it);
	if (g_kept.empty()) { ReleaseSRWLockExclusive(&g_lock); return; }
	const std::string all(text, size);
	for (const lt::KeptRow& k : g_kept)
		for (size_t row : lt::NamedRows(all, k.name)) g_namedBy[lt::Lower(k.name)].insert(p + ", row " + std::to_string(row));
	ReleaseSRWLockExclusive(&g_lock);
}

bool WriteKept();
std::string Stamp();
// Once per start, at the first ItemStatCost read: Live Properties' stat-edits.txt is what the saves
// were written with, so a made stat's cell (signed, send bits, save bits, save add) that differs in
// grown-rows.txt takes stat-edits' value (dated copy of grown-rows.txt first). Under g_lock.
bool g_reconciled = false;
void ReconcileWithStatEdits(std::vector<std::string>& log) {
	if (g_reconciled) return;
	g_reconciled = true;
	OwnFolder();
	const std::string mod = g_ctx->activeMod != nullptr && g_ctx->activeMod[0] != '\0' ? g_ctx->activeMod : "no mod";
	const std::wstring path = g_loaderDir + L"\\live-suite\\live-properties\\" + Widen(mod) + L"\\stat-edits.txt";
	std::string edits;
	if (g_kept.empty() || !ReadWhole(path, edits)) return;
	std::vector<std::string> notes;
	if (!lt::ReconcileSaveShape(g_kept, edits, notes)) return;
	std::string before;
	if (ReadWhole(KeptPath(), before)) WriteOwn(OwnFolder() + L"\\grown-rows.before-reconcile-" + Widen(Stamp()) + L".txt", before);
	const bool ok = WriteKept();
	for (const std::string& n : notes) log.push_back("grown-rows.txt: " + n);
	log.push_back(ok ? "grown-rows.txt written with Live Properties' signed / send / save cells" : "WARNING: grown-rows.txt could not be written (the rows are laid in with stat-edits' cells this start all the same)");
}

void TableText(ReadResult* out) {
	const uint64_t size  = *reinterpret_cast<const uint64_t*>(out->path + 0x10);
	const uint64_t room  = *reinterpret_cast<const uint64_t*>(out->path + 0x18);
	const char*    chars = room >= 16 ? *reinterpret_cast<char* const*>(out->path) : reinterpret_cast<const char*>(out->path);
	if (chars == nullptr || size == 0 || size > 4096 || out->begin == nullptr || out->end < out->begin) return;
	const std::string path(chars, size);
	lt::TableId table;
	int         which = 0;
	if (!WhichTable(path, table, which)) {
		NoteNames(path, reinterpret_cast<const char*>(out->begin), static_cast<size_t>(out->end - out->begin));
		return;
	}
	std::string text(reinterpret_cast<const char*>(out->begin), static_cast<size_t>(out->end - out->begin));
	std::vector<const lt::KeptRow*> rows;
	AcquireSRWLockExclusive(&g_lock);
	ReadKeptOnce();
	for (const lt::KeptRow& k : g_kept)
		if (k.table == table && (which == 0 ? k.versions.find('3') != std::string::npos : k.versions.find_first_of("12") != std::string::npos)) rows.push_back(&k);
	std::vector<std::string> added, notes;
	if (table == lt::TableId::ItemStatCost) ReconcileWithStatEdits(notes);
	std::vector<lt::Placed> placed;
	const size_t before = lt::ReadSheet(text).rows.size();
	if (!rows.empty()) text = lt::AppendKept(text, table, rows, added, notes, &placed);
	for (const lt::Moved& m : lt::MovedRows(placed, table, which)) { // kept once per (table, text, name): D2RCore reads some texts twice
		bool seen = false;
		for (const lt::Moved& o : g_moved) seen = seen || (o.table == m.table && o.which == m.which && lt::Lower(o.name) == lt::Lower(m.name));
		if (!seen) g_moved.push_back(m);
		LogF("table rows %s: %s %s was number %zu when it was made and is %zu this start", path.c_str(), table == lt::TableId::ItemStatCost ? "ALARM" : "note", m.name.c_str(), m.was, m.now);
	}
	Text& t = g_texts[static_cast<int>(table)][which];
	if (t.reads.size() >= 4) t.reads.erase(t.reads.begin()); // a reload reads them again
	t.reads.push_back(lt::ReadSheet(text));
	t.seen = true;
	ReleaseSRWLockExclusive(&g_lock);
	for (const std::string& n : notes) LogF("table rows %s: %s", path.c_str(), n.c_str());
	if (added.empty()) {
		LogF("table text: %s (%zu rows): no rows of Live Tables", path.c_str(), before);
		return;
	}
	g_clearReserve(&out->begin, text.size()); // the game frees the old bytes and gives new room
	if (out->begin == nullptr || static_cast<size_t>(out->capacity - out->begin) < text.size()) {
		LogF("table text: %s: the game gave no room for the added rows - the table is EMPTY now", path.c_str());
		return;
	}
	memcpy(out->begin, text.data(), text.size());
	out->end = out->begin + text.size();
	std::string names;
	for (size_t i = 0; i < added.size(); ++i) names += (i ? ", " : "") + added[i] + " = " + std::to_string(before + i);
	LogF("table text: %s (%zu rows): %zu rows of Live Tables added (%s)", path.c_str(), before, added.size(), names.c_str());
}

uint64_t HookReadText(uint64_t flag, const char* name, const char* ext, ReadResult* out) {
	const uint64_t result = g_readText(flag, name, ext, out);
	if (out != nullptr && out->ok != 0 && ext != nullptr && _stricmp(ext, ".txt") == 0) TableText(out);
	return result;
}

// The detour on D2RCore's read routine: its first 17 bytes (pushes and the stack frame) go to a
// trampoline that jumps back after them.
bool InstallTableHook(std::string& problem) {
	uint8_t* read    = reinterpret_cast<uint8_t*>(g_core) + kReadTextRva;
	uint8_t* reserve = reinterpret_cast<uint8_t*>(g_core) + kClearReserveRva;
	if (!Readable(read, sizeof(kReadTextStart)) || memcmp(read, kReadTextStart, sizeof(kReadTextStart)) != 0) {
		problem = read[0] == 0xff || read[0] == 0xe9 ? "D2RCore's table read routine is hooked by another plugin" : "D2RCore's table read routine is not the one Live Tables knows";
		return false;
	}
	if (!Readable(reserve, sizeof(kClearReserveStart)) || memcmp(reserve, kClearReserveStart, sizeof(kClearReserveStart)) != 0) {
		problem = "D2RCore's vector routine is not the one Live Tables knows";
		return false;
	}
	auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (trampoline == nullptr) { problem = "no memory for the trampoline"; return false; }
	auto jump = [](uint8_t* at, const void* to) { // jmp [rip+0]; dq to
		at[0] = 0xff; at[1] = 0x25; memset(at + 2, 0, 4);
		const uint64_t target = reinterpret_cast<uint64_t>(to);
		memcpy(at + 6, &target, 8);
	};
	memcpy(trampoline, read, sizeof(kReadTextStart));
	jump(trampoline + sizeof(kReadTextStart), read + sizeof(kReadTextStart));
	FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
	g_readText     = reinterpret_cast<ReadTextFn>(trampoline);
	g_clearReserve = reinterpret_cast<ClearReserveFn>(reserve);
	DWORD old = 0;
	if (!VirtualProtect(read, sizeof(kReadTextStart), PAGE_EXECUTE_READWRITE, &old)) { problem = "the routine could not be made writable"; return false; }
	uint8_t patch[sizeof(kReadTextStart)];
	memset(patch, 0x90, sizeof(patch));
	jump(patch, reinterpret_cast<const void*>(&HookReadText));
	memcpy(read, patch, sizeof(patch));
	VirtualProtect(read, sizeof(kReadTextStart), old, &old);
	FlushInstructionCache(GetCurrentProcess(), read, sizeof(kReadTextStart));
	return true;
}

// ---------------------------------------------------------------- listeners

SRWLOCK g_listenLock = SRWLOCK_INIT;
struct Listener { int handle; livetables::ChangeFn fn; void* user; };
std::vector<Listener> g_listeners;
int g_nextHandle = 1;

void Tell(const livetables::Change& c) {
	AcquireSRWLockShared(&g_listenLock);
	const std::vector<Listener> list = g_listeners;
	ReleaseSRWLockShared(&g_listenLock);
	for (const Listener& l : list) l.fn(l.user, &c);
}

// ---------------------------------------------------------------- growing

bool WriteKept() { // under g_lock
	std::string file = kKeptHeader;
	for (const lt::KeptRow& row : g_kept) file += lt::KeptLine(row) + "\n";
	return WriteOwn(KeptPath(), file);
}

std::string Stamp() {
	time_t now = time(nullptr);
	tm t {};
	localtime_s(&t, &now);
	char b[32];
	strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &t);
	return b;
}

struct Request {
	lt::TableId table;
	int         version;     // 0 = all
	std::string text;
	livetables::DoneFn done;
	void*       user;
	enum Kind { Grow, Disable, Enable, Remove, Rename, FixMoved, SetCells } kind = Grow;
	size_t      number = 0;  // Disable / Enable / Remove
	bool        savesFixed = false; // Remove: Save Scan's writer has run for it
	int         loadedHolders = -2; // Remove: the loaded character's items holding it (read in a UI job first); -1 unreadable, -2 not read
	// result
	bool        ok = false;
	int32_t     numbers[4] = { -1, -1, -1, -1 };
	uint32_t    relinked = 0;
	std::string why;
};
std::vector<Request*> g_queue, g_finished; // under g_lock
std::vector<livetables::Change> g_changes;  // under g_lock: told after the lock is let go, still at the safe point
std::deque<std::string> g_renameNames;      // the Rename / CellEdit Change's texts (kept until the next table load)

// The data versions a request grows: 3 alone; 1 and 2 together (they read the same text).
std::vector<int> Targets(int version) {
	std::vector<int> v;
	for (int i = 1; i <= 3; ++i) {
		const bool wanted = version == 0 || (version == 3 ? i == 3 : (version == 1 || version == 2) && i != 3);
		if (wanted && BankUp(i)) v.push_back(i);
	}
	// one data tables object for 1 and 2: grow it once
	if (v.size() >= 2 && v[0] == 1 && v[1] == 2 && g_link.tables(1) == g_link.tables(2)) v.erase(v.begin() + 1);
	return v;
}

// The D2RCore stat-name lists a grown data version's stat goes into: its own; with one data tables
// object for 1 and 2 (grown once), both.
std::vector<int> NameLists(int version) {
	std::vector<int> v { version };
	if (version == 1 && g_link.tables(1) == g_link.tables(2)) v.push_back(2);
	return v;
}

// under g_lock. The check of one data version (nothing changed).
lt::Check CheckIn(lt::TableId table, int version, const std::string& rowText) {
	lt::Check c;
	if (!g_tableHook) {
		c.why = "rows could not be kept for the next start: " + g_hookProblem;
		return c;
	}
	const Vector* vec = VectorOf(table, version);
	void* names = NamesOf(version, lt::NamesOf(table));
	if (names == nullptr) { c.why = "the game has no name table for it"; return c; }
	const int32_t next = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(names) + kNameNextAt);
	Text& text = g_texts[static_cast<int>(table)][TextIndex(version)];
	const lt::Sheet* sheet = text.Matching(vec->count);
	if (sheet == nullptr) {
		c.why = text.seen ? "the table has " + std::to_string(vec->count) + " rows, but no text the game read has that many: something else changed the table"
		                  : "the text the game read for this table was not seen";
		return c;
	}
	c = lt::CheckRow(table, *sheet, vec->count, next, LimitOf(table), rowText, GameResolver(version));
	if (c.ok && table == lt::TableId::ItemStatCost) {
		std::string why;
		if (!g_namesProblem.empty()) why = g_namesProblem;
		for (int lv : NameLists(version)) {
			if (!why.empty()) break;
			AcquireSRWLockShared(CoreNamesLock());
			const ltn::MsvcVector list = *CoreNames(lv);
			ReleaseSRWLockShared(CoreNamesLock());
			const size_t n = ltn::Count(list);
			if (!ltn::Sane(list, why)) break;
			if (n != 0 && n != vec->count) why = "D2RCore's stat-name list of data version " + std::to_string(lv) + " has " + std::to_string(n) + " names, the table " + std::to_string(vec->count) + " rows";
		}
		if (!why.empty()) {
			c.ok  = false;
			c.why = "the new stat's name could not go into D2RCore's environment list: " + why;
		}
	}
	return c;
}

// AT THE SAFE POINT. One request in every data version it asks for.
std::vector<int> g_descPending; // under g_lock: data versions whose description order the UI job redoes
void GrowNow(Request& q, bool uiMayRun);
void OffNow(Request& q, bool uiMayRun);
void RemoveNow(Request& q, bool uiMayRun, bool inGame);
void RenameNow(Request& q);
void SetCellsNow(Request& q, bool inGame);
void RemovePropNow(Request& q);
void GrowNow(Request& q, bool uiMayRun) {
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	std::vector<lt::Check> checks;
	for (int v : targets) {
		checks.push_back(CheckIn(q.table, v, q.text));
		if (!checks.back().ok) { q.why = "data version " + std::to_string(v) + ": " + checks.back().why; return; }
	}
	if (uiMayRun) { // the game's insert rebuilds the bucket array when it is full: not while the UI may look a name up
		for (int v : targets) {
			const uint8_t* names = static_cast<const uint8_t*>(NamesOf(v, lt::NamesOf(q.table)));
			const uint64_t buckets = *reinterpret_cast<const uint64_t*>(names + 0x18), count = *reinterpret_cast<const uint64_t*>(names + 0x28);
			const float most = *reinterpret_cast<const float*>(names + 0x30);
			if (buckets == 0 || static_cast<float>(count + 1) / static_cast<float>(buckets) > most) {
				q.why = "the game's name table would have to rebuild its buckets (" + std::to_string(count) + " names in " + std::to_string(buckets) + "), which is not safe while the UI runs";
				return;
			}
		}
	}
	const size_t rs = lt::RowSizeOf(q.table);
	std::vector<uint8_t*> fresh;
	for (const lt::Check& c : checks) {
		fresh.push_back(static_cast<uint8_t*>(GameAlloc((c.number + 1) * rs)));
		if (fresh.back() == nullptr) {
			for (uint8_t* p : fresh) if (p != nullptr) GameFree(p);
			q.why = "the game's allocator gave no memory";
			return;
		}
	}
	// D2RCore's stat-name lists: made whole to the side now, swapped in after the tables
	struct ListGrow { int version; ltn::Prepared p; size_t before; const uint8_t* was; };
	std::vector<ListGrow> lists;
	if (q.table == lt::TableId::ItemStatCost) {
		AcquireSRWLockShared(CoreNamesLock());
		for (size_t k = 0; k < targets.size() && q.why.empty(); ++k)
			for (int lv : NameLists(targets[k])) {
				const ltn::MsvcVector list = *CoreNames(lv);
				if (ltn::Count(list) == 0) { LogF("D2RCore's stat-name list of data version %d is empty (not read): left as it is", lv); continue; }
				ListGrow g { lv, {}, ltn::Count(list), list.begin };
				std::string why;
				if (!ltn::Prepare(kCoreHeap, list, checks[k].name, g.p, why)) { q.why = "the new stat's name could not go into D2RCore's environment list: " + why; break; }
				lists.push_back(g);
			}
		ReleaseSRWLockShared(CoreNamesLock());
		if (!q.why.empty()) {
			for (ListGrow& g : lists) ltn::Discard(kCoreHeap, g.p);
			for (uint8_t* p : fresh) GameFree(p);
			return;
		}
	}
	std::string versions;
	std::vector<lt::Sheet*> texts; // each text once (1 and 2 share one): the row goes into it after all versions
	for (size_t k = 0; k < targets.size(); ++k) {
		const int v = targets[k];
		Vector* vec = VectorOf(q.table, v);
		uint8_t* next = fresh[k];
		const lt::Check& c = checks[k];
		memcpy(next, vec->rows, c.number * rs);                 // the old rows as they are
		lt::Sheet& sheet = *g_texts[static_cast<int>(q.table)][TextIndex(v)].Matching(c.number); // CheckIn found it
		const lt::Resolver r = GameResolver(v);
		const lt::Grown g = lt::GrowInto(q.table, sheet, c, next, r);
		std::vector<size_t> props;
		if (q.table == lt::TableId::ItemStatCost) {             // older property rows naming the new stat (in place)
			Vector* pv = VectorOf(lt::TableId::Properties, v);
			if (const lt::Sheet* pt = g_texts[1][TextIndex(v)].Matching(pv->count); pt != nullptr)
				props = lt::RelinkCells(lt::TableId::Properties, *pt, pv->rows, pv->count, lt::StatNames, c.name, static_cast<uint16_t>(c.number));
		}
		uint8_t* old = vec->rows;
		const uint64_t oldCount = vec->count;
		const bool gameOwned = vec->capacity >= 0;
		std::atomic_thread_fence(std::memory_order_release);
		*static_cast<uint8_t* volatile*>(&vec->rows) = next;   // begin, then count
		std::atomic_thread_fence(std::memory_order_release);
		*static_cast<volatile uint64_t*>(&vec->count) = c.number + 1;
		vec->capacity = static_cast<int64_t>(c.number + 1);     // the game's memory: it frees it at unload
		g_retired.push_back({ old, gameOwned });
		if (q.table == lt::TableId::ItemStatCost) { // D2RCore frees the old order array: only where no tooltip is being built
			if (!uiMayRun) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
			else if (std::find(g_descPending.begin(), g_descPending.end(), v) == g_descPending.end()) g_descPending.push_back(v);
		}
		if (std::find(texts.begin(), texts.end(), &sheet) == texts.end()) texts.push_back(&sheet);
		lt::ReportNumber(q.numbers, v, c.number, OneObject12());
		q.relinked += static_cast<uint32_t>(g.relinked.size() + props.size());
		versions += std::to_string(v);
		if (v == 1 && g_link.tables(1) == g_link.tables(2)) versions += "2";
		const uint64_t revision = ++g_revision;
		g_changes.push_back({ sizeof(livetables::Change), livetables::ChangeKind::Grow, static_cast<livetables::Table>(q.table), v, old, next, oldCount, c.number + 1, rs, revision });
		if (v == 1 && g_link.tables(1) == g_link.tables(2)) // one data tables object for 1 and 2: told for both
			g_changes.push_back({ sizeof(livetables::Change), livetables::ChangeKind::Grow, static_cast<livetables::Table>(q.table), 2, old, next, oldCount, c.number + 1, rs, revision });
		LogF("grown: %s row %zu \"%s\" in data version %d%s", lt::TableFileName(q.table), c.number, c.name.c_str(), v,
		     g.relinked.size() + props.size() != 0 ? (" (" + std::to_string(g.relinked.size() + props.size()) + " older rows re-linked)").c_str() : "");
	}
	for (lt::Sheet* sheet : texts) sheet->rows.push_back(checks[0].cells);
	if (!lists.empty()) { // all at once under D2RCore's own lock, as D2RCore replaces a list (0x3b745f)
		std::vector<std::pair<uint8_t*, size_t>> old;
		AcquireSRWLockExclusive(CoreNamesLock());
		bool still = true; // nobody replaced a list since it was prepared (D2RCore does only when it reads the tables)
		for (const ListGrow& g : lists) still = still && CoreNames(g.version)->begin == g.was;
		for (ListGrow& g : lists) {
			if (!still) break;
			uint8_t* array = nullptr; size_t bytes = 0;
			ltn::Swap(*CoreNames(g.version), g.p, array, bytes);
			old.push_back({ array, bytes });
		}
		ReleaseSRWLockExclusive(CoreNamesLock());
		if (!still) {
			for (ListGrow& g : lists) ltn::Discard(kCoreHeap, g.p);
			lists.clear();
			LogF("WARNING: D2RCore replaced its stat-name list meanwhile: \"%s\" is not in it (a character saved now may show the environment warning once)", checks[0].name.c_str());
		}
		for (const auto& [array, bytes] : old) ltn::Release(kCoreHeap, array, bytes);
		for (const ListGrow& g : lists) LogF("D2RCore stat-name list, data version %d: %zu -> %zu names (\"%s\" at %zu)", g.version, g.before, g.p.count, checks[0].name.c_str(), g.p.count - 1);
	}
	// kept for the next start
	lt::KeptRow k;
	k.table = q.table; k.versions = versions; k.name = checks[0].name;
	{
		size_t n3 = SIZE_MAX, nb = SIZE_MAX;
		for (size_t t = 0; t < targets.size(); ++t) (targets[t] == 3 ? n3 : nb) = checks[t].number;
		k.number = n3 != SIZE_MAX ? n3 : nb;
		k.numberBase = n3 != SIZE_MAX && nb != SIZE_MAX ? nb : SIZE_MAX;
	}
	const lt::Sheet& sh = *g_texts[static_cast<int>(q.table)][TextIndex(targets[0])].Matching(checks[0].number + 1);
	for (size_t i = 0; i < sh.header.size() && i < checks[0].cells.size(); ++i)
		if (!checks[0].cells[i].empty()) k.cells.push_back({ sh.header[i], checks[0].cells[i] });
	g_kept.push_back(k);
	if (!WriteKept()) {
		q.why = "grown, but grown-rows.txt could not be written: the row will NOT be there at the next start";
		LogF("WARNING: %s", q.why.c_str());
	}
	q.ok = true;
}


// ---------------------------------------------------------------- disable / enable

// The disabled rows, by data version and number (in memory), and by name in disabled-rows.txt.
struct Off { lt::OffCells was; std::string name; };
std::map<std::pair<int, size_t>, Off> g_off; // under g_lock; data version 2 is kept under 1 when they are one object
int OffKey(int version) { return version == 2 && g_link.tables(1) == g_link.tables(2) ? 1 : version; }
std::wstring OffPath() { return OwnFolder() + L"\\disabled-rows.txt"; }
constexpr char kOffHeader[] =
	"# Live Tables: ItemStatCost rows that are disabled (no effect; still saved). Done again every time the game reads its tables.\n"
	"# itemstatcost<TAB>v=<data versions><TAB><stat name>   (close the game before editing)\n";

bool WriteOff() { // under g_lock
	std::map<std::string, std::string> byName; // name -> versions
	for (const auto& [key, off] : g_off) {
		std::string& v = byName[off.name];
		v += std::to_string(key.first);
		if (key.first == 1 && g_link.tables(1) == g_link.tables(2)) v += "2";
	}
	std::string file = kOffHeader;
	for (const auto& [name, versions] : byName) file += "itemstatcost\tv=" + versions + "\t" + name + "\n";
	return WriteOwn(OffPath(), file);
}

constexpr size_t kCubeRow = 0x16c; // D2RCore's cube record (Live Cube Recipes' memory); the vector at datatables+0x1570
const Vector* CubeOf(int v) {
	const auto* vec = reinterpret_cast<const Vector*>(g_link.tables(static_cast<uint8_t>(v)) + 0x1570);
	return Readable(vec, sizeof(Vector)) && vec->rows != nullptr ? vec : nullptr;
}

std::string StatName(int version, size_t n) { // the row's name as the game's text has it
	const Vector* vec = VectorOf(lt::TableId::ItemStatCost, version);
	const lt::Sheet* sh = g_texts[0][TextIndex(version)].Matching(vec->count);
	return sh != nullptr && n < sh->rows.size() ? lt::NameIn(sh->header, sh->rows[n], lt::TableId::ItemStatCost) : std::string();
}

void TellBoth(livetables::Change c) { // under g_lock: queued for after the lock; data version 2 too when it is 1's object
	g_changes.push_back(c);
	if (c.dataVersion == 1 && g_link.tables(1) == g_link.tables(2)) { c.dataVersion = 2; g_changes.push_back(c); }
}

int  OneVersion(int v) { return v >= 1 && v <= 3 ? v : (BankUp(3) ? 3 : BankUp(2) ? 2 : 1); }

// The row `q.number` is in its own data version (q.version; 0 = the newest read), by name: the
// number of that name in every target (data versions 1/2 and 3 number their rows apart).
bool ResolveByName(Request& q, const std::vector<int>& targets, std::string& name, std::vector<size_t>& numbers) {
	const int ti = static_cast<int>(q.table);
	const int base = q.version >= 1 && q.version <= 3 ? q.version : OneVersion(0);
	const Vector* bv = VectorOf(q.table, base);
	if (q.number >= bv->count) { q.why = "data version " + std::to_string(base) + " has no row " + std::to_string(q.number); return false; }
	const lt::Sheet* sh = g_texts[ti][TextIndex(base)].Matching(bv->count);
	name = sh != nullptr ? lt::NameIn(sh->header, sh->rows[q.number], q.table) : std::string();
	if (name.empty()) { q.why = "the text the game read for data version " + std::to_string(base) + " does not match its table"; return false; }
	numbers.clear();
	for (int v : targets) {
		void* names = NamesOf(v, lt::NamesOf(q.table));
		const int32_t n = names != nullptr ? g_link.lookup(names, name.c_str(), 0) : -1;
		if (n < 0 || static_cast<uint64_t>(n) >= VectorOf(q.table, v)->count) { q.why = name + " is not in data version " + std::to_string(v); return false; }
		numbers.push_back(static_cast<size_t>(n));
	}
	return true;
}

// AT THE JOB. Disable / Enable of one ItemStatCost row in every data version asked for.
void OffNow(Request& q, bool uiMayRun) {
	if (q.table != lt::TableId::ItemStatCost) { q.why = "only ItemStatCost rows are disabled here (Live Properties disables properties)"; return; }
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	std::string rowName;
	std::vector<size_t> numbers;
	if (!ResolveByName(q, targets, rowName, numbers)) return;
	const bool off = q.kind == Request::Disable;
	std::vector<lt::VersionRow> rows;
	if (!lt::PairVersions(targets, numbers, rows)) { q.why = "the row was not found in every data version"; return; }
	for (const lt::VersionRow& r : rows) {
		const int v = r.version;
		const size_t number = r.number;
		Vector* vec = VectorOf(q.table, v);
		const auto key = std::make_pair(OffKey(v), number);
		const bool was = g_off.count(key) != 0;
		if (off == was) { lt::ReportNumber(q.numbers, v, number, OneObject12()); continue; } // already so
		if (off) g_off[key] = { lt::DisableStat(vec->rows, vec->count, number), StatName(v, number) };
		else { lt::EnableStat(vec->rows, vec->count, number, g_off[key].was); g_off.erase(key); }
		if (!uiMayRun) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
		else if (std::find(g_descPending.begin(), g_descPending.end(), v) == g_descPending.end()) g_descPending.push_back(v);
		lt::ReportNumber(q.numbers, v, number, OneObject12());
		livetables::Change ch { sizeof(livetables::Change), off ? livetables::ChangeKind::Disable : livetables::ChangeKind::Enable, livetables::Table::ItemStatCost, v,
		                        vec->rows, vec->rows, vec->count, vec->count, lt::kStatRow, ++g_revision, number, nullptr, 0 };
		TellBoth(ch);
		LogF("%s: itemstatcost row %zu \"%s\" in data version %d", off ? "disabled" : "enabled", number, StatName(v, number).c_str(), v);
	}
	if (!WriteOff()) q.why = "done, but disabled-rows.txt could not be written: it will NOT be so at the next start";
	q.ok = true;
}

// At every table read (DataTablesLoaded): the rows of disabled-rows.txt disabled again, by name.
void ReapplyOff() { // under g_lock
	g_off.clear();
	std::string file;
	if (!ReadWhole(OffPath(), file)) return;
	size_t at = 0, done = 0;
	std::set<int> versions;
	while (at < file.size()) {
		size_t end = file.find('\n', at);
		if (end == std::string::npos) end = file.size();
		std::string line = file.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
		if (t2 == std::string::npos || line.compare(0, t1, "itemstatcost") != 0 || line.compare(t1 + 1, 2, "v=") != 0) continue;
		const std::string vs = line.substr(t1 + 3, t2 - t1 - 3), name = line.substr(t2 + 1);
		for (char c : vs) {
			const int v = c - '0';
			if (v < 1 || v > 3 || !BankUp(v)) continue;
			void* names = NamesOf(v, lt::StatNames);
			const int32_t n = names != nullptr ? g_link.lookup(names, name.c_str(), 0) : -1;
			Vector* vec = VectorOf(lt::TableId::ItemStatCost, v);
			if (n < 0 || static_cast<uint64_t>(n) >= vec->count) { LogF("disabled-rows.txt: %s is not a stat in data version %d (left out)", name.c_str(), v); continue; }
			const auto key = std::make_pair(OffKey(v), static_cast<size_t>(n));
			if (g_off.count(key)) continue;
			g_off[key] = { lt::DisableStat(vec->rows, vec->count, static_cast<size_t>(n)), name };
			versions.insert(OffKey(v));
			++done;
		}
	}
	for (int v : versions) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
	if (done != 0) LogF("disabled-rows.txt: %zu rows disabled again", done);
}

// ---------------------------------------------------------------- remove


std::vector<std::vector<int32_t>> g_maps; // the maps handed to listeners (kept until the next table load); under g_lock

// The loaded character's items: does one hold stat n? (-1: cannot tell here)
D2RL::Inventory::IterationAction __cdecl CollectItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept {
	static_cast<std::vector<D2RL::ItemHandle>*>(userData)->push_back(item->handle);
	return D2RL::Inventory::IterationAction::Continue;
}
// The loaded character's items holding stat n: read in a UI job (the SDK captures the local player
// on the UI side) before the remove is queued. -1 = could not be read (the step is logged).
int LoadedHolders(size_t n) {
	if (g_items == nullptr || g_inventory == nullptr) { LogF("remove check: no item / inventory service"); return -1; }
	if (!D2RL::HasItemServiceField(g_items, D2RL::ItemServiceDetailsFieldEnd)
		|| !D2RL::HasInventoryServiceField(g_inventory, offsetof(D2RL::InventoryService, forEachInventoryItem) + sizeof(void*))) { LogF("remove check: this D2RLoader's item / inventory service lacks getItemDetails / forEachInventoryItem"); return -1; }
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	const auto rp = g_inventory->getLocalPlayer(g_ctx, &player);
	if (rp != D2RL::Inventory::Result::Success || player == D2RL::InvalidPlayerHandle) { LogF("remove check: getLocalPlayer gave result %u (no loaded character?)", static_cast<unsigned>(rp)); return -1; }
	uint32_t mask = 0;
	for (uint32_t c = 1; c <= 9; ++c) mask |= D2RL::Items::ContainerBit(static_cast<D2RL::Items::ItemContainer>(c));
	const D2RL::Inventory::ItemFilter filter { .structSize = D2RL::Inventory::ItemFilterSize, .containerMask = mask };
	std::vector<D2RL::ItemHandle> items;
	const auto rf = g_inventory->forEachInventoryItem(g_ctx, player, &filter, CollectItem, &items);
	if (rf != D2RL::Inventory::Result::Success) { LogF("remove check: forEachInventoryItem gave result %u", static_cast<unsigned>(rf)); return -1; }
	int holders = 0;
	size_t read = 0;
	std::vector<D2RL::Items::ItemStatValue> stats(64);
	for (const D2RL::ItemHandle item : items) {
		D2RL::Items::ItemDetails details { .structSize = D2RL::Items::ItemDetailsSize };
		auto rd = g_items->getItemDetails(g_ctx, item, &details, stats.data(), static_cast<uint32_t>(stats.size()));
		if (rd == D2RL::Items::Result::InvalidArgument && details.statCount > stats.size()) { // capacity: the call says how many
			stats.resize(details.statCount);
			details = { .structSize = D2RL::Items::ItemDetailsSize };
			rd = g_items->getItemDetails(g_ctx, item, &details, stats.data(), static_cast<uint32_t>(stats.size()));
		}
		if (rd != D2RL::Items::Result::Success) { LogF("remove check: getItemDetails gave result %u for one of %zu items", static_cast<unsigned>(rd), items.size()); return -1; }
		++read;
		for (uint32_t i = 0; i < details.statCount && i < stats.size(); ++i) if (stats[i].statId == n) { ++holders; break; }
	}
	LogF("remove check: the loaded character's %zu items read (all containers but the ground): %d hold stat %zu", read, holders, n);
	return holders;
}

// Save Scan's writer for a remove: called at the job, BEFORE the table changes; its done() comes on
// Save Scan's thread. While it runs Live Tables holds every other change (g_fixing).
std::atomic<bool> g_fixing { false };
void Start();
void __cdecl FixFile(void*, const savescan::FileReport* r) noexcept {
	if (r != nullptr) LogF("save fix: %s: state %d, %u items, %u changed, %u entries dropped", r->file != nullptr ? r->file : "?", static_cast<int>(r->state), r->items, r->itemsChanged, r->dropped);
}

// The name table node of `name` out of its chain (kept: a reader may stand on it), numbers above n
// one lower, the counters one lower. Game layout: see table_model.h NameTable.
bool UnnameStat(void* table, const std::string& name, size_t n, bool renumber) {
	auto* t = static_cast<uint8_t*>(table);
	const uint64_t buckets = *reinterpret_cast<uint64_t*>(t + 0x18);
	auto** heads = *reinterpret_cast<uint8_t***>(t + 0x20);
	if (buckets == 0 || heads == nullptr) return false;
	const std::string key = lt::Lower(name);
	uint64_t h[2];
	lt::Murmur3(key.data(), key.size(), 0, h);
	uint8_t** at = &heads[h[0] & (buckets - 1)];
	bool gone = false;
	while (*at != nullptr) {
		uint8_t* node = *at;
		const char* text = *reinterpret_cast<const char**>(node + 0x8);
		const uint64_t len = *reinterpret_cast<const uint64_t*>(node + 0x10);
		if (len == key.size() && text != nullptr && memcmp(text, key.data(), len) == 0) {
			*at = *reinterpret_cast<uint8_t**>(node); // one pointer store; the node stays as it is
			gone = true;
			break;
		}
		at = reinterpret_cast<uint8_t**>(node);
	}
	if (!gone) return false;
	--*reinterpret_cast<uint64_t*>(t + 0x28);
	if (renumber)
		for (uint64_t b = 0; b < buckets; ++b)
			for (uint8_t* node = heads[b]; node != nullptr; node = *reinterpret_cast<uint8_t**>(node)) {
				int32_t& num = *reinterpret_cast<int32_t*>(node + 0x30);
				if (num > static_cast<int32_t>(n)) --num;
			}
	--*reinterpret_cast<int32_t*>(t + 0x38);
	int32_t& empty = *reinterpret_cast<int32_t*>(t + 0x3c);
	if (empty > static_cast<int32_t>(n)) --empty;
	return true;
}


// The remove journal: live-suite\live-tables\<mod>\remove-in-progress.txt, written before Save
// Scan fixes the saves and deleted when the remove is done. Left over at a start: the remove did not
// end. When grown-rows.txt no longer has the row, it did end (only the delete was missed); else
// the saves are put back from the backup it names (Save Scan's restore).
std::wstring JournalPath() { return OwnFolder() + L"\\remove-in-progress.txt"; }
void Journal(const std::string& name, size_t n, const std::string& backup) {
	WriteOwn(JournalPath(), lt::JournalLine(name, n, backup));
}
void DropJournal() { DeleteFileW(JournalPath().c_str()); }

// Every item unit the game holds, server and client: which hold stat n? Read-only, at the game job.
struct Walk { bool ok = false; uintptr_t game = 0; int version = -1; size_t server = 0, client = 0, holders = 0; std::string why; };
bool ListHolds(const uint8_t* list, size_t n) {
	if (list == nullptr || !Readable(list, 0xb8)) return false;
	auto scan = [&](uintptr_t at) {
		const uint8_t* rows = *reinterpret_cast<uint8_t* const*>(list + at);
		const uint64_t count = *reinterpret_cast<const uint64_t*>(list + at + 8);
		if (rows == nullptr || count == 0 || count > 0x4000 || !Readable(rows, count * 16)) return false;
		for (uint64_t i = 0; i < count; ++i) if ((*reinterpret_cast<const uint64_t*>(rows + i * 16) >> 32) == n) return true;
		return false;
	};
	return scan(0x30) || (*reinterpret_cast<const int32_t*>(list + 0x1c) < 0 && scan(0xa8));
}
size_t WalkBuckets(const uint8_t* buckets, size_t n, size_t& holders, bool& bad) {
	size_t items = 0, steps = 0;
	if (!Readable(buckets, 128 * 8)) { bad = true; return 0; }
	for (int b = 0; b < 128; ++b)
		for (const uint8_t* u = *reinterpret_cast<uint8_t* const*>(buckets + b * 8); u != nullptr; u = *reinterpret_cast<uint8_t* const*>(u + 0x158)) {
			if (++steps > 500000 || !Readable(u, 0x160)) { bad = true; return items; }
			if (*reinterpret_cast<const uint32_t*>(u) != 4) continue; // unit type 4 = item
			++items;
			if (ListHolds(*reinterpret_cast<uint8_t* const*>(u + 0x88), n)) ++holders;
		}
	return items;
}
Walk WalkItems(size_t n) {
	Walk w;
	for (const Print& p : kWalkPrints)
		if (memcmp(reinterpret_cast<const uint8_t*>(g_core) + p.rva, p.bytes, p.size) != 0) { w.why = std::string("D2RCore's code is not what Live Tables expects at ") + p.what; return w; }
	const uint8_t* session = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(g_core) + 0x700480);
	if (session == nullptr || !Readable(session + 0x2e0, 8)) { w.why = "no session"; return w; }
	const uint8_t* game = *reinterpret_cast<uint8_t* const*>(session + 0x2e0);
	w.game = reinterpret_cast<uintptr_t>(game);
	if (game == nullptr || !Readable(game, 0x3640)) { w.why = "no game"; return w; }
	w.version = game[0x106];
	bool bad = false;
	w.server = WalkBuckets(game + 0x2E30, n, w.holders, bad);
	w.client = WalkBuckets(reinterpret_cast<const uint8_t*>(g_imageBase + 0x2A23910 + 4 * 0x400), n, w.holders, bad);
	if (bad) { w.why = "a unit chain could not be read to its end"; return w; }
	w.ok = true;
	return w;
}

// AT THE JOB. Remove one row Live Tables grew (ItemStatCost), in every data version asked for.
void RemoveNow(Request& q, bool uiMayRun, bool inGame) {
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	const size_t n = q.number;
	std::string name;
	size_t count = 0;
	for (int v : targets) {
		const Vector* vec = VectorOf(q.table, v);
		if (n >= vec->count) { q.why = "data version " + std::to_string(v) + " has no row " + std::to_string(n); return; }
		const std::string nm = StatName(v, n);
		if (nm.empty()) { q.why = "the text the game read for data version " + std::to_string(v) + " does not match its table"; return; }
		if (!name.empty() && lt::Lower(nm) != lt::Lower(name)) { q.why = "row " + std::to_string(n) + " is not the same stat in every data version"; return; }
		name = nm;
		if (count != 0 && count != vec->count) { q.why = "the data versions have different row counts: remove it in one at a time"; return; }
		count = vec->count;
	}
	bool kept = false;
	for (const lt::KeptRow& k : g_kept) kept = kept || (k.table == q.table && lt::Lower(k.name) == lt::Lower(name));
	if (!kept) { q.why = name + " is not a row Live Tables grew: only those are removed"; return; }
	const bool middle = n + 1 < count;
	if (middle && inGame) { q.why = "leave to the menu to remove a middle row (" + name + " has " + std::to_string(count - 1 - n) + " rows after it); it can be disabled now"; return; }
	// what in the tables still names it
	for (int v : targets) {
		const Vector* sv = VectorOf(lt::TableId::ItemStatCost, v);
		const Vector* pv = VectorOf(lt::TableId::Properties, v);
		const std::vector<std::string> named = lt::StatNamedBy(sv->rows, sv->count, n, pv->rows, pv->count);
		if (!named.empty()) {
			std::string list;
			for (size_t i = 0; i < named.size() && i < 6; ++i) list += (i ? ", " : "") + named[i];
			q.why = name + " is still named by " + list + (named.size() > 6 ? " ..." : "") + " (data version " + std::to_string(v) + ")";
			return;
		}
	}
	// cube recipes: a record's param (+0x4) with an op is a stat number (Live Cube Recipes recompiles them, but not before this change)
	for (int v : targets)
		if (const Vector* cube = CubeOf(v); cube != nullptr)
			for (uint64_t r = 0; r < cube->count; ++r) {
				const uint8_t* rec = cube->rows + r * kCubeRow;
				const uint32_t param = *reinterpret_cast<const uint32_t*>(rec + 0x4);
				if (rec[0x3] != 0 && param >= n && param < count) {
					q.why = "cube recipe " + std::to_string(r) + " compares stat " + std::to_string(param) + (param == n ? " (this one)" : " (whose number would change)");
					return;
				}
			}
	for (size_t r = n; r < count; ++r) { // the row, and (a middle row) every row whose number moves
		const std::string nm = lt::Lower(r == n ? name : StatName(targets[0], r));
		const auto it = g_namedBy.find(nm);
		if (it != g_namedBy.end() && !it->second.empty()) {
			q.why = std::string(r == n ? name : "row " + std::to_string(r) + " (" + nm + "), which would become one lower,") + " is named in " + *it->second.begin()
			      + (it->second.size() > 1 ? " and " + std::to_string(it->second.size() - 1) + " more places" : "") + ": that table holds its number and is not rewritten live";
			return;
		}
		bool keptToo = r == n;
		for (const lt::KeptRow& k : g_kept) keptToo = keptToo || (k.table == q.table && lt::Lower(k.name) == nm);
		if (!keptToo) { q.why = "row " + std::to_string(r) + " after it is not a row Live Tables grew"; return; }
	}
	// saves
	const savescan::Api* scan = savescan::Find(1);
	if (scan == nullptr) { q.why = "Save Scan (d2rl-save-scan.dll) is needed: the saves must be read before a stat is removed"; return; }
	if (inGame) {
		savescan::Summary sum { sizeof(sum) };
		const uint16_t stat = static_cast<uint16_t>(n);
		scan->scan(&stat, 1, &sum, nullptr, nullptr, nullptr);
		if (!savescan::Safe(sum)) {
			q.why = sum.ran == 0 || sum.unreadable != 0 ? "the saves could not all be read: leave to the menu and remove it there"
			                                           : std::to_string(sum.holders) + " saved items hold " + name + ": leave to the menu, the saves are fixed there";
			return;
		}
		const int held = q.loadedHolders;
		if (held != 0) { q.why = held < 0 ? "the loaded character's items could not be read (see live-tables.log): leave to the menu and remove it there" : std::to_string(held) + " items of the loaded character hold " + name + ": leave to the menu, the save is fixed there"; return; }
		// every item unit (ground, vendors, mercenary, cube ...): walked and logged; refused in a game until this walk is proven in a log
		const Walk w = WalkItems(n);
		static bool saidGame = false;
		if (!saidGame || !w.ok) {
			saidGame = true;
			LogF("unit walk: game pointer %p (data version byte %d), %zu server item units, %zu client item units, %zu hold stat %zu%s%s", reinterpret_cast<void*>(w.game), w.version, w.server, w.client, w.holders, n,
			     w.ok ? "" : " - not read to the end: ", w.why.c_str());
		} else LogF("unit walk: %zu server and %zu client item units, %zu hold stat %zu", w.server, w.client, w.holders, n);
		q.why = "a last-row remove in a game waits until the item walk is proven (this one read " + std::to_string(w.server) + " server and " + std::to_string(w.client)
		      + " client items, " + std::to_string(w.holders) + " holding it" + (w.ok ? "" : "; " + w.why) + "): leave to the menu and remove it there";
		return;
	}
	// At the menu: Save Scan's writer, in THIS job (the UI is parked here: no game can start), only
	// after every check above. Without the version-3 writer: allowed only when no save holds it.
	const savescan::Api* writer = inGame ? nullptr : savescan::Find(3);
	std::vector<savescan::StatChange> changes;
	if (!inGame) {
		if (writer == nullptr) {
			savescan::Summary sum { sizeof(sum) };
			const uint16_t stat = static_cast<uint16_t>(n);
			scan->scan(&stat, 1, &sum, nullptr, nullptr, nullptr);
			if (middle || !savescan::Safe(sum)) { q.why = "Save Scan 0.3.4 (its save writer and restore) is needed to " + std::string(middle ? "renumber the saves" : "take the stat off the saved items"); return; }
		} else {
			changes.push_back({ static_cast<uint16_t>(n), savescan::kDropped });
			for (size_t r = n + 1; r < count; ++r) changes.push_back({ static_cast<uint16_t>(r), static_cast<int32_t>(r - 1) });
		}
	}
	// D2RCore's stat-name lists without the name, made whole first
	struct ListCut { int version; ltn::Prepared p; };
	std::vector<ListCut> cuts;
	AcquireSRWLockShared(CoreNamesLock());
	for (int v : targets)
		for (int lv : NameLists(v)) {
			const ltn::MsvcVector list = *CoreNames(lv);
			if (ltn::Count(list) != count) continue; // not read (empty): left as it is
			ListCut c { lv, {} };
			std::string why;
			if (!ltn::PrepareWithout(kCoreHeap, list, n, c.p, why)) { q.why = why; break; }
			cuts.push_back(c);
		}
	ReleaseSRWLockShared(CoreNamesLock());
	if (!q.why.empty()) { for (ListCut& c : cuts) ltn::DiscardWithout(kCoreHeap, c.p); return; }
	// new memory for a middle row (every row after it moves), the same memory for the last
	std::vector<uint8_t*> fresh;
	if (middle)
		for (size_t k = 0; k < targets.size(); ++k) {
			fresh.push_back(static_cast<uint8_t*>(GameAlloc((count - 1) * lt::kStatRow)));
			if (fresh.back() == nullptr) {
				for (uint8_t* p : fresh) if (p != nullptr) GameFree(p);
				for (ListCut& c : cuts) ltn::DiscardWithout(kCoreHeap, c.p);
				q.why = "the game's allocator gave no memory";
				return;
			}
		}
	auto giveBack = [&]() {
		for (uint8_t* p : fresh) if (p != nullptr) GameFree(p);
		for (ListCut& c : cuts) ltn::DiscardWithout(kCoreHeap, c.p);
	};
	// Live Tables' files as they will be after the remove (written before the table changes)
	std::vector<lt::KeptRow> keptNext;
	for (const lt::KeptRow& k0 : g_kept) {
		lt::KeptRow k = k0;
		if (k.table == q.table && lt::Lower(k.name) == lt::Lower(name)) continue;
		if (k.table == q.table && k.number != SIZE_MAX && k.number > n) --k.number;
		if (k.table == q.table && k.numberBase != SIZE_MAX && k.numberBase > n) --k.numberBase;
		keptNext.push_back(k);
	}
	std::map<std::pair<int, size_t>, Off> offNext;
	for (auto& [key, o] : g_off) if (key.second != n) offNext[{ key.first, key.second > n ? key.second - 1 : key.second }] = o;
	std::string backup;
	if (writer != nullptr) {
		// the journal first: if the game stops between here and the end, the next start puts it right
		Journal(name, n, "");
		savescan::FixRequest fr { sizeof(fr) };
		fr.changes = changes.data(); fr.changeCount = static_cast<uint32_t>(changes.size());
		fr.file = FixFile;
		savescan::FixDone out {};
		out.size = sizeof(out);
		LogF("remove %s (%zu): Save Scan fixes the saves first (%zu stat numbers change)", name.c_str(), n, changes.size());
		if (writer->fixSavesNow(&fr, &out) != 1 || out.ok != 1) {
			q.why = std::string("Save Scan could not fix the saves, so nothing changed: ") + (out.why != nullptr ? out.why : "no answer");
			if (out.notRestored > 0) LogF("WARNING: %s", q.why.c_str());
			else DropJournal();
			giveBack();
			return;
		}
		backup = out.backup != nullptr ? out.backup : "";
		Journal(name, n, backup);
		LogF("remove %s: the saves are fixed (%u files, %u items; backup %s)", name.c_str(), out.changed, out.itemsChanged, backup.c_str());
	}
	{
		std::string before;
		if (ReadWhole(KeptPath(), before)) WriteOwn(OwnFolder() + L"\\grown-rows.before-remove-" + Widen(Stamp()) + L".txt", before);
		std::swap(g_kept, keptNext);
		std::swap(g_off, offNext);
		if (!WriteKept() || !WriteOff()) {
			std::swap(g_kept, keptNext); // back as it was
			std::swap(g_off, offNext);
			WriteKept();
			WriteOff();
			q.why = "Live Tables' files could not be written, so the table is unchanged";
			if (writer != nullptr && !backup.empty()) {
				savescan::FixDone out {};
				out.size = sizeof(out);
				const bool back = writer->restore(backup.c_str(), &out) == 1 && out.ok == 1;
				q.why += back ? " and the saves were put back" : std::string("; the saves could NOT all be put back: ") + (out.why != nullptr ? out.why : "") + " (backup " + backup + ")";
				if (back) DropJournal();
			}
			LogF("WARNING: remove %s: %s", name.c_str(), q.why.c_str());
			giveBack();
			return;
		}
	}
	// from here nothing can fail: the tables change
	g_maps.emplace_back(count);
	std::vector<int32_t>& map = g_maps.back();
	for (size_t r = 0; r < count; ++r) map[r] = r < n ? static_cast<int32_t>(r) : r == n ? -1 : static_cast<int32_t>(r - 1);
	std::vector<lt::Sheet*> texts;
	for (size_t k = 0; k < targets.size(); ++k) {
		const int v = targets[k];
		Vector* vec = VectorOf(q.table, v);
		lt::Sheet* sheet = g_texts[0][TextIndex(v)].Matching(count);
		uint8_t* old = vec->rows;
		if (middle) {
			lt::RemoveStatRows(old, count, n, fresh[k]);
			std::atomic_thread_fence(std::memory_order_release);
			*static_cast<volatile uint64_t*>(&vec->count) = count - 1;  // count first, then begin: a reader checking either sees rows that exist
			std::atomic_thread_fence(std::memory_order_release);
			*static_cast<uint8_t* volatile*>(&vec->rows) = fresh[k];
			g_retired.push_back({ old, vec->capacity >= 0 });
			vec->capacity = static_cast<int64_t>(count - 1);
			Vector* pv = VectorOf(lt::TableId::Properties, v);
			lt::RenumberPropStats(pv->rows, pv->count, n);
		} else {
			lt::UnlinkStat(old, count, n);
			std::atomic_thread_fence(std::memory_order_release);
			*static_cast<volatile uint64_t*>(&vec->count) = count - 1;  // the row's bytes stay where they are
		}
		UnnameStat(NamesOf(v, lt::StatNames), name, n, middle);
		if (!uiMayRun) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
		else if (std::find(g_descPending.begin(), g_descPending.end(), v) == g_descPending.end()) g_descPending.push_back(v);
		if (sheet != nullptr && std::find(texts.begin(), texts.end(), sheet) == texts.end()) texts.push_back(sheet);
		lt::ReportNumber(q.numbers, v, n, OneObject12());
		livetables::Change ch { sizeof(livetables::Change), middle ? livetables::ChangeKind::Remove : livetables::ChangeKind::Shrink, livetables::Table::ItemStatCost, v,
		                        old, vec->rows, count, count - 1, lt::kStatRow, ++g_revision, n, map.data(), map.size() };
		TellBoth(ch);
	}
	for (lt::Sheet* sheet : texts) sheet->rows.erase(sheet->rows.begin() + static_cast<long>(n));
	if (!cuts.empty()) {
		std::vector<std::pair<uint8_t*, size_t>> old;
		AcquireSRWLockExclusive(CoreNamesLock());
		for (ListCut& c : cuts) { uint8_t* a = nullptr; size_t b = 0; ltn::Swap(*CoreNames(c.version), c.p, a, b); old.push_back({ a, b }); }
		ReleaseSRWLockExclusive(CoreNamesLock());
		for (auto& [a, b] : old) ltn::Release(kCoreHeap, a, b);
		for (ListCut& c : cuts) { ltn::FreeRemovedText(kCoreHeap, c.p); LogF("D2RCore stat-name list, data version %d: %zu -> %zu names", c.version, count, count - 1); }
	}
	DropJournal();
	LogF("removed: itemstatcost row %zu \"%s\" (%s)%s", n, name.c_str(), middle ? "a middle row: the rows after it are one lower" : "the last row",
	     writer != nullptr ? "; the saves were fixed first, in the same job" : "");
	q.ok = true;
}


// ---------------------------------------------------------------- tables that hold property numbers
// A "mod" cell (parser 0x3203e0) is a dword {u16 id, u16 kind}: kind 0 = property, 1 = property
// group, 0xffffffff = none. Places (loader column tables): affixes +0x15e8 rows 0x8c, mod1-3 at
// +0x24/+0x34/+0x44; cube +0x1570 rows 0x16c, 3 outputs x 5 mods (D2RCore's record: 0x77 + 0x5b*o +
// 0xc*m); unique items +0x13c8 rows 0x15c, prop1-12 at +0x98 + 0x10*k; runes +0x1860 rows 0xe8,
// t1code1-7 at +0x74 + 0x10*k. Other tables (sets, set items, gems, quality items, property groups)
// are covered by the name scan of their texts at load.
bool HoldsProp(const uint8_t* cell, size_t n) {
	uint16_t id, kind;
	memcpy(&id, cell, 2); memcpy(&kind, cell + 2, 2);
	return kind == 0 && id == n;
}
std::string PropNamedInMemory(int v, size_t n) {
	const uint8_t* t = g_link.tables(static_cast<uint8_t>(v));
	struct Place { uintptr_t at; size_t row; std::vector<uint32_t> cells; const char* what; };
	std::vector<Place> places = {
		{ 0x15e8, 0x8c, { 0x24, 0x34, 0x44 }, "affix" },
		{ 0x13c8, 0x15c, {}, "unique item" },
		{ 0x1860, 0xe8, {}, "runeword" },
		{ 0x1570, kCubeRow, {}, "cube recipe" },
	};
	for (uint32_t k = 0; k < 12; ++k) places[1].cells.push_back(0x98 + 0x10 * k);
	for (uint32_t k = 0; k < 7; ++k) places[2].cells.push_back(0x74 + 0x10 * k);
	for (uint32_t o = 0; o < 3; ++o) for (uint32_t m = 0; m < 5; ++m) places[3].cells.push_back(0x77 + 0x5b * o + 0xc * m);
	for (const Place& p : places) {
		const auto* vec = reinterpret_cast<const Vector*>(t + p.at);
		if (!Readable(vec, sizeof(Vector)) || vec->rows == nullptr) continue;
		for (uint64_t r = 0; r < vec->count; ++r)
			for (uint32_t c : p.cells)
				if (HoldsProp(vec->rows + r * p.row + c, n)) return std::string(p.what) + " " + std::to_string(r);
	}
	return {};
}

// AT THE JOB. The last Properties row Live Tables grew, out.
void RemovePropNow(Request& q) {
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	std::string code;
	std::vector<size_t> numbers;
	if (!ResolveByName(q, targets, code, numbers)) return;
	for (size_t t = 0; t < targets.size(); ++t) {
		const int v = targets[t];
		const size_t n = numbers[t];
		const Vector* vec = VectorOf(q.table, v);
		if (n + 1 != vec->count) { q.why = "only the last property row can be removed for now (" + code + " has " + std::to_string(vec->count - 1 - n) + " rows after it in data version " + std::to_string(v) + ")"; return; }
		const std::string held = PropNamedInMemory(v, n);
		if (!held.empty()) { q.why = code + " is still named by " + held + " (data version " + std::to_string(v) + ")"; return; }
	}
	bool kept = false;
	for (const lt::KeptRow& k : g_kept) kept = kept || (k.table == q.table && lt::Lower(k.name) == lt::Lower(code));
	if (!kept) { q.why = code + " is not a row Live Tables grew: only those are removed"; return; }
	if (const auto it = g_namedBy.find(lt::Lower(code)); it != g_namedBy.end() && !it->second.empty()) {
		q.why = code + " is named in " + *it->second.begin() + ": that table holds its number and is not rewritten live";
		return;
	}
	std::vector<lt::Sheet*> texts;
	for (size_t t = 0; t < targets.size(); ++t) {
		const int v = targets[t];
		const size_t n = numbers[t];
		g_maps.emplace_back(n + 1);
		std::vector<int32_t>& map = g_maps.back();
		for (size_t r = 0; r <= n; ++r) map[r] = r < n ? static_cast<int32_t>(r) : -1;
		Vector* vec = VectorOf(q.table, v);
		lt::Sheet* sheet = g_texts[1][TextIndex(v)].Matching(vec->count);
		std::atomic_thread_fence(std::memory_order_release);
		*static_cast<volatile uint64_t*>(&vec->count) = n; // the row's bytes stay where they are
		UnnameStat(NamesOf(v, lt::PropNames), code, n, false);
		if (sheet != nullptr && std::find(texts.begin(), texts.end(), sheet) == texts.end()) texts.push_back(sheet);
		lt::ReportNumber(q.numbers, v, n, OneObject12());
		livetables::Change ch { sizeof(livetables::Change), livetables::ChangeKind::Shrink, livetables::Table::Properties, v,
		                        vec->rows, vec->rows, n + 1, n, lt::kPropRow, ++g_revision, n, map.data(), map.size() };
		TellBoth(ch);
	}
	for (lt::Sheet* sheet : texts) sheet->rows.pop_back();
	std::string before;
	if (ReadWhole(KeptPath(), before)) WriteOwn(OwnFolder() + L"\\grown-rows.before-remove-" + Widen(Stamp()) + L".txt", before);
	g_kept.erase(std::remove_if(g_kept.begin(), g_kept.end(), [&](const lt::KeptRow& k) { return k.table == q.table && lt::Lower(k.name) == lt::Lower(code); }), g_kept.end());
	if (!WriteKept()) q.why = "removed, but grown-rows.txt could not be written: the row comes BACK at the next start";
	LogF("removed: properties \"%s\" (the last row; number %zu in data version %d)", code.c_str(), numbers[0], targets[0]);
	q.ok = true;
}

// The name node of `was` gets `now`: the old node out of its chain (kept), the new name put in with
// the game's own insert under the same number (its counter set to it for the call and put back).
bool RenameNode(void* table, const std::string& was, const std::string& now, size_t n) {
	auto* t = static_cast<uint8_t*>(table);
	int32_t& next = *reinterpret_cast<int32_t*>(t + 0x38);
	const int32_t keepNext = next, keepEmpty = *reinterpret_cast<int32_t*>(t + 0x3c);
	if (!UnnameStat(table, was, n, false)) return false;    // count -1, next -1 (put back below)
	next = static_cast<int32_t>(n);
	const int32_t got = g_link.insert(table, now.c_str(), true);
	next = keepNext;
	*reinterpret_cast<int32_t*>(t + 0x3c) = keepEmpty;
	return got == static_cast<int32_t>(n);
}

// AT THE JOB. Rename a row Live Tables grew.
void RenameNow(Request& q) {
	const std::string now = q.text;
	if (now.empty() || now.size() > 0x2f || now.find_first_of("\t\r\n") != std::string::npos) { q.why = "the new name must be 1-47 letters, no tabs"; return; }
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	std::string was;
	std::vector<size_t> numbers;
	if (!ResolveByName(q, targets, was, numbers)) return;
	for (int v : targets)
		if (g_link.lookup(NamesOf(v, lt::NamesOf(q.table)), now.c_str(), 0) >= 0) { q.why = "the name " + now + " is taken"; return; }
	bool kept = false;
	for (const lt::KeptRow& k : g_kept) kept = kept || (k.table == q.table && lt::Lower(k.name) == lt::Lower(was));
	if (!kept) { q.why = was + " is not a row Live Tables grew: only those are renamed"; return; }
	if (const auto it = g_namedBy.find(lt::Lower(was)); it != g_namedBy.end() && !it->second.empty()) {
		q.why = was + " is named in " + *it->second.begin() + ": renamed, that table would lose it at the next start";
		return;
	}
	// D2RCore's stat-name lists: made whole first
	std::vector<std::pair<int, ltn::Replaced>> lists;
	if (q.table == lt::TableId::ItemStatCost) {
		AcquireSRWLockShared(CoreNamesLock());
		for (size_t t = 0; t < targets.size() && q.why.empty(); ++t)
			for (int lv : NameLists(targets[t])) {
				const ltn::MsvcVector list = *CoreNames(lv);
				if (ltn::Count(list) != VectorOf(q.table, targets[t])->count) continue;
				ltn::Replaced r; std::string why;
				if (!ltn::PrepareReplace(kCoreHeap, list, numbers[t], now, r, why)) { q.why = why; break; }
				lists.push_back({ lv, r });
			}
		ReleaseSRWLockShared(CoreNamesLock());
		if (!q.why.empty()) { for (auto& [lv, r] : lists) ltn::DiscardReplace(kCoreHeap, r); return; }
	}
	for (size_t t = 0; t < targets.size(); ++t) {
		const int v = targets[t];
		const size_t number = numbers[t];
		if (!RenameNode(NamesOf(v, lt::NamesOf(q.table)), was, now, number)) LogF("WARNING: rename %s -> %s: the name table of data version %d did not take it", was.c_str(), now.c_str(), v);
		lt::ReportNumber(q.numbers, v, number, OneObject12());
		const Vector* vec = VectorOf(q.table, v);
		livetables::Change ch { sizeof(livetables::Change), livetables::ChangeKind::Rename, static_cast<livetables::Table>(q.table), v,
		                        vec->rows, vec->rows, vec->count, vec->count, lt::RowSizeOf(q.table), ++g_revision, number, nullptr, 0, nullptr, nullptr };
		TellBoth(ch);
	}
	if (!lists.empty()) {
		std::vector<std::pair<uint8_t*, size_t>> old;
		AcquireSRWLockExclusive(CoreNamesLock());
		for (auto& [lv, r] : lists) { uint8_t* a = nullptr; size_t b = 0; ltn::Swap(*CoreNames(lv), r.p, a, b); old.push_back({ a, b }); }
		ReleaseSRWLockExclusive(CoreNamesLock());
		for (auto& [a, b] : old) ltn::Release(kCoreHeap, a, b);
		for (auto& [lv, r] : lists) ltn::FreeOldText(kCoreHeap, r);
	}
	// the texts in memory, the kept rows, the disabled rows
	for (int t = 0; t < 2; ++t) for (int w = 0; w < 2; ++w) for (lt::Sheet& sh : g_texts[t][w].reads) lt::RenameInSheet(sh, static_cast<lt::TableId>(t), q.table, was, now);
	std::string before;
	if (ReadWhole(KeptPath(), before)) WriteOwn(OwnFolder() + L"\\grown-rows.before-rename-" + Widen(Stamp()) + L".txt", before);
	const size_t cells = lt::RenameInKept(g_kept, q.table, was, now);
	if (q.table == lt::TableId::ItemStatCost) for (auto& [key, off] : g_off) if (lt::Lower(off.name) == lt::Lower(was)) off.name = now;
	if (const auto it = g_namedBy.find(lt::Lower(was)); it != g_namedBy.end()) { g_namedBy[lt::Lower(now)] = it->second; g_namedBy.erase(it); }
	if (!WriteKept() || !WriteOff()) q.why = "renamed, but Live Tables' files could not be written: the old name comes BACK at the next start";
	// the Change's names: valid during the listener call (told after the lock, still in this job)
	g_renameNames.push_back(was);
	g_renameNames.push_back(now);
	for (livetables::Change& c : g_changes) if (c.kind == livetables::ChangeKind::Rename && c.oldName == nullptr) { c.oldName = g_renameNames[g_renameNames.size() - 2].c_str(); c.newName = g_renameNames.back().c_str(); }
	LogF("renamed: %s row %zu \"%s\" -> \"%s\" (%zu cells of kept rows followed)", lt::TableFileName(q.table), q.number, was.c_str(), now.c_str(), cells);
	q.ok = true;
}


// At a start (DataTablesLoaded, at the menu): a remove journal left over.
void RecoverJournal() {
	std::string file;
	if (!ReadWhole(JournalPath(), file)) return;
	std::string line;
	for (size_t at = 0; at < file.size();) {
		size_t end = file.find('\n', at); if (end == std::string::npos) end = file.size();
		std::string l = file.substr(at, end - at); at = end + 1;
		if (!l.empty() && l.back() == '\r') l.pop_back();
		if (!l.empty() && l[0] != '#') line = l;
	}
	AcquireSRWLockExclusive(&g_lock);
	ReadKeptOnce(); // the read hook may be off: the journal is judged against the file itself
	const bool keptThere = g_keptFileRead;
	const lt::JournalStep step = lt::DecideJournal(file, g_kept);
	ReleaseSRWLockExclusive(&g_lock);
	const std::string& name = step.name;
	const std::string& backup = step.backup;
	std::string alarm;
	if (!keptThere && step.what != lt::JournalStep::None) { // not judged: the journal stays
		alarm = "a change of " + name + " was in progress (remove-in-progress.txt) and grown-rows.txt cannot be read, so whether it ended is not known; the journal is kept and no save was touched (Save Scan backup: " + (backup.empty() ? std::string("none yet") : backup) + ")";
	} else if (step.what == lt::JournalStep::Ended) { DropJournal(); LogF("remove journal: the remove of %s had ended (only the journal was left); removed", name.c_str()); return; }
	if (!alarm.empty()) {
	} else if (step.what == lt::JournalStep::StoppedInFix) {
		alarm = "a stat remove of " + name + " stopped while Save Scan was fixing the saves: open Save Scan's Saves tab and restore, before any character is loaded";
	} else if (const savescan::Api* writer = savescan::Find(3); writer == nullptr) {
		alarm = "a stat remove of " + name + " stopped after the saves were fixed and Save Scan 0.3.4 is not here to put them back: copy them back from " + backup + " before any character is loaded";
	} else {
		savescan::FixDone out {};
		out.size = sizeof(out);
		if (writer->restore(backup.c_str(), &out) == 1 && out.ok == 1) {
			DropJournal();
			LogF("remove journal: the remove of %s had stopped after the saves were fixed; the saves were put back (%u files) from %s", name.c_str(), out.restored, backup.c_str());
			return;
		}
		alarm = "a stat remove of " + name + " stopped after the saves were fixed, and they could not all be put back (" + std::string(out.why != nullptr ? out.why : "") + "): backup " + backup;
	}
	LogF("ALARM: %s", alarm.c_str());
	AcquireSRWLockExclusive(&g_lock);
	g_alarm = g_alarm.empty() ? alarm : g_alarm + ". Also: " + alarm;
	ReleaseSRWLockExclusive(&g_lock);
	livetables::Change ch {};
	ch.size = sizeof(ch); ch.kind = livetables::ChangeKind::Alarm; ch.table = livetables::Table::ItemStatCost; ch.revision = ++g_revision;
	Tell(ch);
}

// AT A MENU JOB. The saves follow the made stats that moved at this start (the alarm).
void FixMovedNow(Request& q, bool inGame) {
	if (inGame) { q.why = "leave to the menu: the saves are renumbered there, before any character is loaded"; return; }
	if (g_movedStats.empty()) { q.why = "no made stat moved at this start"; q.ok = true; return; }
	const savescan::Api* writer = savescan::Find(3);
	if (writer == nullptr) { q.why = "Save Scan 0.3.4 (its save writer) is needed"; return; }
	std::map<size_t, size_t> move; // was -> now (one pair for every text it moved in)
	for (const lt::Moved& m : g_movedStats) {
		const auto it = move.find(m.was);
		if (it != move.end() && it->second != m.now) { q.why = m.name + " moved differently in data versions 1/2 and 3 (" + std::to_string(m.was) + " -> " + std::to_string(it->second) + " / " + std::to_string(m.now) + "): one save fix cannot do both"; return; }
		move[m.was] = m.now;
	}
	const int v = BankUp(3) ? 3 : 1;
	const Vector* vec = VectorOf(lt::TableId::ItemStatCost, v);
	std::vector<savescan::StatChange> changes;
	std::vector<savescan::Resize> before;
	for (const auto& [was, now] : move) // a number the saves did not use as made: dropped, so the moved ones cannot collide (Save Scan's rule)
		if (move.find(now) == move.end()) changes.push_back({ static_cast<uint16_t>(now), savescan::kDropped });
	for (const auto& [was, now] : move) {
		if (now >= vec->count) { q.why = "row " + std::to_string(now) + " is not in the table"; return; }
		const uint8_t* r = vec->rows + now * lt::kStatRow;
		changes.push_back({ static_cast<uint16_t>(was), static_cast<int32_t>(now) });
		before.push_back({ static_cast<uint16_t>(was), r[0x15], static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(r + 0x20)), *reinterpret_cast<const int32_t*>(r + 0x18) });
	}
	savescan::FixRequest fr { sizeof(fr) };
	fr.changes = changes.data(); fr.changeCount = static_cast<uint32_t>(changes.size());
	fr.before = before.data(); fr.beforeCount = static_cast<uint32_t>(before.size());
	fr.notAfter = g_startTime;
	fr.file = FixFile;
	savescan::FixDone out {};
	out.size = sizeof(out);
	if (writer->fixSavesNow(&fr, &out) != 1 || out.ok != 1) { q.why = std::string("Save Scan could not renumber the saves, so nothing changed: ") + (out.why != nullptr ? out.why : "no answer"); return; }
	const std::string backup = out.backup != nullptr ? out.backup : "";
	std::vector<lt::KeptRow> keptBefore = g_kept;
	for (const lt::Moved& m : g_movedStats)
		for (lt::KeptRow& k : g_kept) {
			if (k.table != m.table || lt::Lower(k.name) != lt::Lower(m.name)) continue;
			const bool in3 = k.versions.find('3') != std::string::npos;
			if (m.which == 0) k.number = m.now;
			else if (k.numberBase != SIZE_MAX) k.numberBase = m.now;
			else if (!in3) k.number = m.now;
		}
	if (!WriteKept()) {
		g_kept = keptBefore;
		savescan::FixDone back {};
		back.size = sizeof(back);
		const bool ok = writer->restore(backup.c_str(), &back) == 1 && back.ok == 1;
		q.why = std::string("grown-rows.txt could not be written; the saves ") + (ok ? "were put back" : "could NOT all be put back (backup " + backup + ")");
		return;
	}
	LogF("fixmoved: the saves follow the made stats that moved (%zu numbers, %u files changed; backup %s); the alarm is down", move.size(), out.changed, backup.c_str());
	g_movedStats.clear();
	g_alarm.clear();
	livetables::Change ch {};
	ch.size = sizeof(ch); ch.kind = livetables::ChangeKind::Alarm; ch.table = livetables::Table::ItemStatCost; ch.revision = ++g_revision;
	g_changes.push_back(ch);
	q.ok = true;
}


// AT THE JOB. Cells of a row Live Tables grew.
void SetCellsNow(Request& q, bool inGame) {
	const std::vector<int> targets = Targets(q.version);
	if (targets.empty()) { q.why = "the game has not read that data version's tables"; return; }
	std::string name;
	std::vector<size_t> numbers;
	if (!ResolveByName(q, targets, name, numbers)) return;
	lt::KeptRow* kept = nullptr;
	for (lt::KeptRow& k : g_kept) if (k.table == q.table && lt::Lower(k.name) == lt::Lower(name)) kept = &k;
	if (kept == nullptr) { q.why = name + " is not a row Live Tables grew: only those take cells here"; return; }
	const int ti = static_cast<int>(q.table);
	// the cells asked for
	std::vector<std::pair<std::string, std::string>> cells;
	{
		std::string text = q.text;
		for (size_t c = 0; c <= text.size();) {
			size_t t = text.find('\t', c); if (t == std::string::npos) t = text.size();
			const std::string part = text.substr(c, t - c); c = t + 1;
			if (part.empty()) continue;
			const size_t eq = part.find('=');
			if (eq == std::string::npos) { q.why = "a cell is not column=value: " + part; return; }
			cells.push_back({ part.substr(0, eq), part.substr(eq + 1) });
		}
	}
	if (cells.empty()) { q.why = "no cells"; return; }
	const lt::Sheet* sheet0 = g_texts[ti][TextIndex(targets[0])].Matching(VectorOf(q.table, targets[0])->count);
	if (sheet0 == nullptr) { q.why = "the text the game read does not match the table"; return; }
	bool opChanged = false, descChanged = false, resize = false, saveOther = false;
	std::vector<std::pair<const lt::Column*, bool>> shapeCols; // a save-shape cell asked for: resize (true) or other
	for (auto& [col, val] : cells) {
		const lt::Column* c = nullptr;
		for (const lt::Column& k : lt::ColumnsOf(q.table)) if (lt::Lower(col) == k.name) c = &k;
		bool inHeader = false;
		for (const std::string& h : sheet0->header) if (lt::Lower(h) == lt::Lower(col)) { col = h; inHeader = true; }
		if (c == nullptr || !inHeader) { q.why = "no column " + col + " in " + lt::TableFileName(q.table) + " (as the game read it)"; return; }
		if (c->kind == lt::KName) { q.why = "the name is changed with rename"; return; }
		if (c->kind == lt::KString && !val.empty() && g_stringIdOr != nullptr) {
			struct { const char* p; size_t n; } key { val.c_str(), val.size() };
			if (g_stringIdOr(&key, 0xffff) == 0xffff) { q.why = "unknown string key " + val + " (" + col + ")"; return; }
		}
		if (q.table == lt::TableId::ItemStatCost) {
			const std::string l = lt::Lower(col);
			if (l == "op" || l == "op param" || l == "op base" || l.rfind("op stat", 0) == 0) opChanged = true;
			if (l == "descfunc" || l == "descpriority" || l == "descval") descChanged = true;
			if (l == "save bits" || l == "save add" || l == "save param bits") shapeCols.push_back({c, true});
			if (l == "signed" || l == "valshift" || l == "saved" || l == "1.09-save bits" || l == "1.09-save add" || l == "csvbits" || l == "csvparam" || l == "csvsigned") shapeCols.push_back({c, false});
		}
	}
	if (q.table == lt::TableId::ItemStatCost)
		for (size_t t = 0; t < targets.size(); ++t)
			if (g_off.count({ OffKey(targets[t]), numbers[t] })) { q.why = name + " is disabled: enable it first"; return; }
	// the merged row, compiled the game's way (checked before anything changes)
	lt::KeptRow next = *kept;
	lt::SetKeptCells(next, cells);
	std::vector<std::vector<uint8_t>> rows;
	for (size_t t = 0; t < targets.size(); ++t) {
		const lt::Sheet* sh = g_texts[ti][TextIndex(targets[t])].Matching(VectorOf(q.table, targets[t])->count);
		std::vector<std::string> row(sh->header.size());
		for (const auto& [c, v] : next.cells) for (size_t i = 0; i < sh->header.size(); ++i) if (lt::Lower(sh->header[i]) == lt::Lower(c)) row[i] = v;
		rows.emplace_back(lt::RowSizeOf(q.table));
		lt::CompileRow(sh->header, row, q.table, rows.back().data(), GameResolver(targets[t])); // the name exists: pass 1 gives its own number
		uint16_t own; memcpy(&own, rows.back().data(), 2);
		if (own != numbers[t]) { q.why = "the compiled row does not keep its number"; return; }
	}
	// A save-shape cell counts as changed only when its compiled value differs from the row IN
	// MEMORY (what the loaded character and the saves were written with), not from the kept text:
	// a value memory already has is only recorded in grown-rows.txt, no save is touched.
	for (const auto& [c, isResize] : shapeCols)
		for (size_t t = 0; t < targets.size(); ++t) {
			const uint8_t* mem = VectorOf(q.table, targets[t])->rows + numbers[t] * lt::RowSizeOf(q.table);
			const uint8_t* fresh = rows[t].data();
			bool differs;
			switch (c->kind) {
				case lt::KBit:  differs = ((*reinterpret_cast<const uint32_t*>(mem + c->offset) ^ *reinterpret_cast<const uint32_t*>(fresh + c->offset)) >> c->n & 1) != 0; break;
				case lt::KByte: differs = mem[c->offset] != fresh[c->offset]; break;
				case lt::KWord: differs = memcmp(mem + c->offset, fresh + c->offset, 2) != 0; break;
				default:        differs = memcmp(mem + c->offset, fresh + c->offset, 4) != 0; break;
			}
			if (differs) (isResize ? resize : saveOther) = true;
		}
	if (!shapeCols.empty() && !resize && !saveOther) LogF("cells of %s: the save-shape cells asked for are what memory holds already: recorded, no save touched", name.c_str());
	// how items are saved: Save Scan, at the menu, before anything else changes
	const savescan::Api* writer = nullptr;
	std::string backup;
	if (resize || saveOther) {
		if (inGame) { q.why = "a cell of how items are saved changes: leave to the menu, the saves are fixed there"; return; }
		std::vector<lt::VersionRow> vr;
		if (!lt::PairVersions(targets, numbers, vr) || lt::SameNumber(vr) == SIZE_MAX) {
			q.why = name + " has different numbers in the data versions asked for: a cell of how items are saved is changed in one data version at a time";
			return;
		}
		const savescan::Api* scan = savescan::Find(1);
		if (scan == nullptr) { q.why = "Save Scan is needed: the saves must be read before a saved shape changes"; return; }
		savescan::Summary sum { sizeof(sum) };
		const uint16_t stat = static_cast<uint16_t>(numbers[0]);
		scan->scan(&stat, 1, &sum, nullptr, nullptr, nullptr);
		if (sum.ran == 0 || sum.unreadable != 0) { q.why = "the saves could not all be read"; return; }
		if (sum.holders != 0) {
			if (saveOther) { q.why = std::to_string(sum.holders) + " saved items hold " + name + ": signed / valshift / saved / 1.09 / csv cells cannot be fixed in saves"; return; }
			writer = savescan::Find(3);
			if (writer == nullptr) { q.why = "Save Scan 0.3.4 (its resize) is needed: " + std::to_string(sum.holders) + " saved items hold " + name; return; }
			const uint8_t* r = rows[0].data();
			const savescan::Resize rz { stat, r[0x15], static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(r + 0x20)), *reinterpret_cast<const int32_t*>(r + 0x18) };
			WriteOwn(JournalPath(), lt::CellsJournalLine(name, numbers[0], "", cells));
			savescan::FixRequest fr { sizeof(fr) };
			fr.resizes = &rz; fr.resizeCount = 1; fr.file = FixFile;
			savescan::FixDone out {};
			out.size = sizeof(out);
			if (writer->fixSavesNow(&fr, &out) != 1 || out.ok != 1) {
				q.why = std::string("Save Scan could not resize the saves, so nothing changed: ") + (out.why != nullptr ? out.why : "no answer");
				if (out.notRestored == 0) DropJournal();
				return;
			}
			backup = out.backup != nullptr ? out.backup : "";
			WriteOwn(JournalPath(), lt::CellsJournalLine(name, numbers[0], backup, cells));
			LogF("cells of %s: the saves were resized first (%u files, %u items; backup %s)", name.c_str(), out.changed, out.itemsChanged, backup.c_str());
		}
	}
	// the kept row (the one source of a made row's cells), then the table
	{
		std::string before;
		if (ReadWhole(KeptPath(), before)) WriteOwn(OwnFolder() + L"\\grown-rows.before-cells-" + Widen(Stamp()) + L".txt", before);
		const lt::KeptRow was = *kept;
		*kept = next;
		if (!WriteKept()) {
			*kept = was;
			WriteKept();
			q.why = "grown-rows.txt could not be written, so nothing changed";
			if (writer != nullptr && !backup.empty()) {
				savescan::FixDone out {};
				out.size = sizeof(out);
				const bool ok = writer->restore(backup.c_str(), &out) == 1 && out.ok == 1;
				q.why += ok ? " (the saves were put back)" : " - the saves could NOT all be put back: backup " + backup;
				if (ok) DropJournal();
			}
			return;
		}
	}
	for (size_t t = 0; t < targets.size(); ++t) {
		const int v = targets[t];
		const size_t n = numbers[t];
		Vector* vec = VectorOf(q.table, v);
		uint8_t* row = vec->rows + n * lt::RowSizeOf(q.table);
		const uint8_t* fresh = rows[t].data();
		if (q.table == lt::TableId::ItemStatCost) {
			if (opChanged) lt::UnlinkStat(vec->rows, vec->count, n);
			uint8_t keep[0xe0];
			memcpy(keep, row + 0x5a, 0xe0);              // the lists other rows made (+0x5a..+0x139)
			const uint8_t marks[3] = { row[0x4d], row[0x4e], row[0x4f] }, flags = row[4];
			memcpy(row, fresh, lt::kStatRow);
			memcpy(row + 0x5a, keep, 0xe0);
			row[0x4d] = marks[0]; row[0x4e] = marks[1]; row[0x4f] = marks[2];
			row[4] = static_cast<uint8_t>((row[4] & 0x0f) | (flags & 0xf0));
			if (opChanged) lt::LinkStat(vec->rows, vec->count, n, true);
			if (descChanged || opChanged) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
		} else {
			memcpy(row, fresh, lt::kPropRow);
		}
		for (lt::Sheet& sh : g_texts[ti][TextIndex(v)].reads)
			if (n < sh.rows.size() && lt::Lower(lt::NameIn(sh.header, sh.rows[n], q.table)) == lt::Lower(name))
				for (const auto& [c, val] : cells) for (size_t i = 0; i < sh.header.size(); ++i) if (lt::Lower(sh.header[i]) == lt::Lower(c)) { if (sh.rows[n].size() <= i) sh.rows[n].resize(i + 1); sh.rows[n][i] = val; }
		lt::ReportNumber(q.numbers, v, n, OneObject12());
		g_renameNames.push_back(q.text);
		livetables::Change ch { sizeof(livetables::Change), livetables::ChangeKind::CellEdit, static_cast<livetables::Table>(q.table), v,
		                        vec->rows, vec->rows, vec->count, vec->count, lt::RowSizeOf(q.table), ++g_revision, n, nullptr, 0, nullptr, nullptr, g_renameNames.back().c_str() };
		TellBoth(ch);
	}
	DropJournal();
	LogF("cells: %s row %zu \"%s\": %s%s", lt::TableFileName(q.table), numbers[0], name.c_str(), q.text.c_str(), backup.empty() ? "" : " (saves resized first)");
	q.ok = true;
}

// ---- the safe point (see live_tables_api.h) ----
enum State : int { Idle = 0, Queued, Working };
std::atomic<int>      g_state { Idle };
std::atomic<uint64_t> g_round { 0 };
HANDLE g_timerQueue = nullptr;
std::atomic<HANDLE> g_timer { nullptr };
// Which OS thread runs the UI jobs (learnt from every UI job). First run (2026-11-25 21:02-21:05):
// every grow in a game waited 2 s in a game job for a UI job that never came. Either the loader runs
// both on one thread, or the UI's frame waits for the game's update; both are handled below.
std::atomic<DWORD> g_uiThread { 0 };
bool g_saidThreads = false;
void SeenUi() { g_uiThread.store(GetCurrentThreadId()); }
void __cdecl ProbeJob(const D2RL::PluginContext*, void*) noexcept { SeenUi(); }
void DropTimer() {
	if (HANDLE h = g_timer.exchange(nullptr); h != nullptr) DeleteTimerQueueTimer(g_timerQueue, h, nullptr);
}

void __cdecl DoneJob(const D2RL::PluginContext*, void*) noexcept {
	SeenUi();
	std::vector<Request*> done;
	AcquireSRWLockExclusive(&g_lock);
	done.swap(g_finished);
	ReleaseSRWLockExclusive(&g_lock);
	for (Request* q : done) {
		static const char* kWhat[] = { "grown", "disabled", "enabled", "removed", "renamed", "renumbered (saves)", "changed (cells)" };
		if (!q->ok) LogF("not %s: %s row %s: %s", kWhat[q->kind], lt::TableFileName(q->table), q->kind == Request::Grow ? ("(" + q->text.substr(0, 60) + ")").c_str() : std::to_string(q->number).c_str(), q->why.c_str());
		livetables::GrowDone d {};
		d.size = sizeof(d);
		d.ok = q->ok ? 1 : 0;
		memcpy(d.numbers, q->numbers, sizeof(d.numbers));
		d.relinked = q->relinked;
		d.why = q->why.c_str();
		q->done(q->user, &d);
		delete q;
	}
}

void Finish(std::vector<Request*>& list, const char* why) { // under g_lock
	for (Request* q : list) {
		if (why != nullptr) q->why = why;
		g_finished.push_back(q);
	}
	list.clear();
}

void PostDone() {
	if (g_threads->runOnUiThread(g_ctx, DoneJob, nullptr) != D2RL::Threads::Result::Success) DoneJob(g_ctx, nullptr);
}

void Start();

// uiMayRun: the UI thread may be running meanwhile (a game job on another thread). Everything done
// then is safe for a reader on the UI thread: rows in new memory published begin-then-count (old
// buffer kept), a name appended at the tail of a bucket chain (no bucket rebuild), D2RCore's name list
// under its own lock. The description order (D2RCore frees its old array) waits for a UI job.
SRWLOCK g_vetoLock = SRWLOCK_INIT;
struct Veto { int handle; livetables::CanRemoveFn fn; void* user; };
std::vector<Veto> g_vetoes;
int g_nextVeto = 1;
// The vetoes asked again in the job, with no Live Tables lock held (a veto may take its own lock):
// a file may have changed between the remove() call and this job.
std::string AskVetoes(const Request& q) {
	AcquireSRWLockShared(&g_vetoLock);
	const std::vector<Veto> list = g_vetoes;
	ReleaseSRWLockShared(&g_vetoLock);
	for (const Veto& v : list) {
		char why[512] = "";
		if (v.fn(v.user, static_cast<livetables::Table>(q.table), q.version, q.number == SIZE_MAX ? -1 : static_cast<int>(q.number), why, sizeof(why)) == 0)
			return why[0] != '\0' ? why : "another plugin keeps a file that names it";
	}
	return {};
}

void Work(bool uiMayRun, bool inGame) {
	AcquireSRWLockExclusive(&g_lock);
	std::vector<Request*> pending;
	pending.swap(g_queue);
	ReleaseSRWLockExclusive(&g_lock);
	std::vector<Request*> vetoed;
	for (Request* q : pending)
		if (q->kind == Request::Remove) { q->why = AskVetoes(*q); if (!q->why.empty()) vetoed.push_back(q); }
	AcquireSRWLockExclusive(&g_lock);
	std::vector<Request*> work;
	for (Request* q : pending) if (std::find(vetoed.begin(), vetoed.end(), q) == vetoed.end()) work.push_back(q);
	for (Request* q : g_queue) work.push_back(q); // came in meanwhile
	g_queue.clear();
	Finish(vetoed, nullptr);
	std::vector<Request*> finished;
	for (size_t i = 0; i < work.size(); ++i) {
		Request* q = work[i];
		if (g_fixing.load()) { g_queue.insert(g_queue.end(), work.begin() + static_cast<long>(i), work.end()); break; } // Save Scan works: the rest waits
		if (!g_alarm.empty() && q->table == lt::TableId::ItemStatCost && q->kind != Request::FixMoved) { q->why = g_alarm; finished.push_back(q); continue; }
		if (uiMayRun && q->kind != Request::Grow && q->kind != Request::FixMoved) { // in place, while a UI frame may read: only growth is publish-safe
			q->why = "the game and UI jobs run on different threads here: only growing is safe then (disable, remove and rename are not)";
			finished.push_back(q);
			continue;
		}
		switch (q->kind) {
		case Request::Grow: GrowNow(*q, uiMayRun); break;
		case Request::Disable: case Request::Enable: OffNow(*q, uiMayRun); break;
		case Request::Remove: if (q->table == lt::TableId::Properties) RemovePropNow(*q); else RemoveNow(*q, uiMayRun, inGame); break;
		case Request::Rename: RenameNow(*q); break;
		case Request::FixMoved: FixMovedNow(*q, inGame); break;
		case Request::SetCells: SetCellsNow(*q, inGame); break;
		}
		if (q->why == "\x01waiting") { q->why.clear(); continue; } // FixDone hands it back
		finished.push_back(q);
	}
	Finish(finished, nullptr);
	std::vector<livetables::Change> changes;
	changes.swap(g_changes);
	ReleaseSRWLockExclusive(&g_lock);
	for (const livetables::Change& c : changes) Tell(c);
}

void AfterRound() {
	DropTimer();
	g_state.store(Idle);
	PostDone();
	AcquireSRWLockShared(&g_lock);
	const bool more = !g_queue.empty();
	ReleaseSRWLockShared(&g_lock);
	if (more) Start();
}

void Busy(const char* why) {
	AcquireSRWLockExclusive(&g_lock);
	std::vector<Request*> work;
	work.swap(g_queue);
	Finish(work, why);
	ReleaseSRWLockExclusive(&g_lock);
	DropTimer();
	g_state.store(Idle);
	PostDone();
}

// The description order of the grown stat tables, on the UI thread (no tooltip is being built
// while a UI job runs), then the done() calls.
void __cdecl DescJob(const D2RL::PluginContext*, void*) noexcept {
	SeenUi();
	AcquireSRWLockExclusive(&g_lock);
	std::vector<int> versions;
	versions.swap(g_descPending);
	ReleaseSRWLockExclusive(&g_lock);
	for (int v : versions) if (BankUp(v)) g_link.descOrder(g_link.tables(static_cast<uint8_t>(v)));
	DoneJob(g_ctx, nullptr);
}

// The round: one game job (inside the authoritative update: no item creation, no loot roll runs
// meanwhile). Nobody waits for another thread, so nothing can hold up a frame.
void __cdecl GameJob(const D2RL::PluginContext*, void* round) noexcept {
	int expect = Queued;
	if (reinterpret_cast<uint64_t>(round) != g_round.load() || !g_state.compare_exchange_strong(expect, Working)) return;
	const DWORD here = GetCurrentThreadId();
	const DWORD ui = g_threadsKnown ? *reinterpret_cast<const volatile DWORD*>(reinterpret_cast<uint8_t*>(g_core) + kUiThreadIdRva) : g_uiThread.load();
	const bool uiMayRun = ui == 0 || ui != here; // not known: the careful way
	if (!g_saidThreads) {
		g_saidThreads = true;
		const DWORD game = g_threadsKnown ? *reinterpret_cast<const volatile DWORD*>(reinterpret_cast<uint8_t*>(g_core) + kGameThreadIdRva) : 0;
		LogF("safe point: this game job runs on thread %lu; the loader's UI thread is %lu, its game thread %lu (%s)", here, ui, game,
		     uiMayRun ? "two threads: the UI may run meanwhile; the description order follows in a UI job" : "one thread: no UI update can run meanwhile");
	}
	Work(uiMayRun, true);
	DropTimer();
	g_state.store(Idle);
	AcquireSRWLockShared(&g_lock);
	const bool desc = !g_descPending.empty(), more = !g_queue.empty();
	ReleaseSRWLockShared(&g_lock);
	if (!desc || g_threads->runOnUiThread(g_ctx, DescJob, nullptr) != D2RL::Threads::Result::Success) {
		if (desc) LogF("WARNING: the UI thread takes no job: the description order is redone at the next growth or table load");
		PostDone();
	}
	if (more) Start();
}

void __cdecl MenuJob(const D2RL::PluginContext*, void* round) noexcept { // no game thread: the UI thread alone
	SeenUi();
	int expect = Queued;
	if (reinterpret_cast<uint64_t>(round) != g_round.load() || !g_state.compare_exchange_strong(expect, Working)) return;
	Work(false, false);
	AfterRound();
}

void CALLBACK TooLate(void* round, BOOLEAN) { // one shot, 3 s after a round was queued and no job ran it
	int expect = Queued;
	if (reinterpret_cast<uint64_t>(round) != g_round.load() || !g_state.compare_exchange_strong(expect, Idle)) return;
	++g_round;
	LogF("safe point: no job of the round ran within 3 s");
	Busy("busy, try again (the game thread did not run Live Tables' job within 3 s)");
}

void Start() {
	if (g_fixing.load()) return; // Save Scan's writer runs; FixDone starts the next round
	int expect = Idle;
	if (!g_state.compare_exchange_strong(expect, Queued)) return; // a round runs; it takes the queue too
	void* round = reinterpret_cast<void*>(++g_round);
	HANDLE timer = nullptr;
	const bool game = g_threads->runOnGameThread(g_ctx, GameJob, round) == D2RL::Threads::Result::Success;
	if (game || g_threads->runOnUiThread(g_ctx, MenuJob, round) == D2RL::Threads::Result::Success) {
		if (CreateTimerQueueTimer(&timer, g_timerQueue, TooLate, round, 3000, 0, WT_EXECUTEONLYONCE)) {
			DropTimer();
			g_timer.store(timer);
		}
	} else {
		Busy("busy, try again (no thread takes a job now)");
	}
}

// ---------------------------------------------------------------- the API

bool OkTable(livetables::Table t) { return t == livetables::Table::ItemStatCost || t == livetables::Table::Properties; }

int __cdecl ApiCheck(livetables::Table table, int dataVersion, const char* rowText, char* why, size_t whySize) noexcept {
	std::string reason;
	bool ok = false;
	if (!g_link.ok) reason = "Live Tables is off: " + g_linkProblem;
	else if (!OkTable(table) || rowText == nullptr) reason = "no such table";
	else {
		AcquireSRWLockShared(&g_lock);
		const std::vector<int> targets = Targets(dataVersion);
		if (targets.empty()) reason = "the game has not read that data version's tables";
		ok = !targets.empty();
		for (int v : targets) {
			const lt::Check c = CheckIn(static_cast<lt::TableId>(table), v, rowText);
			if (!c.ok) { ok = false; reason = "data version " + std::to_string(v) + ": " + c.why; break; }
		}
		ReleaseSRWLockShared(&g_lock);
	}
	if (why != nullptr && whySize != 0) snprintf(why, whySize, "%s", reason.c_str());
	return ok ? 1 : 0;
}

int __cdecl ApiGrow(livetables::Table table, int dataVersion, const char* rowText, livetables::DoneFn done, void* user) noexcept {
	if (done == nullptr) return 0;
	auto* q = new Request { static_cast<lt::TableId>(table), dataVersion, rowText != nullptr ? rowText : "", done, user };
	AcquireSRWLockExclusive(&g_lock);
	if (!g_link.ok || g_threads == nullptr || !OkTable(table)) {
		q->why = !g_link.ok ? "Live Tables is off: " + g_linkProblem : "no such table";
		g_finished.push_back(q);
		ReleaseSRWLockExclusive(&g_lock);
		if (g_threads != nullptr) PostDone(); else DoneJob(g_ctx, nullptr);
		return 1;
	}
	g_queue.push_back(q);
	ReleaseSRWLockExclusive(&g_lock);
	Start();
	return 1;
}

int __cdecl ApiCount(livetables::Table table, int dataVersion) noexcept {
	const int v = OneVersion(dataVersion);
	return g_link.ok && OkTable(table) && BankUp(v) ? static_cast<int>(VectorOf(static_cast<lt::TableId>(table), v)->count) : -1;
}
int __cdecl ApiLimit(livetables::Table table, int) noexcept {
	return g_link.ok && OkTable(table) ? static_cast<int>(LimitOf(static_cast<lt::TableId>(table))) : -1;
}
int __cdecl ApiFind(livetables::Table table, int dataVersion, const char* name) noexcept {
	const int v = OneVersion(dataVersion);
	if (!g_link.ok || !OkTable(table) || name == nullptr || !BankUp(v)) return -1;
	void* names = NamesOf(v, lt::NamesOf(static_cast<lt::TableId>(table)));
	return names != nullptr ? g_link.lookup(names, name, 0) : -1;
}
uint64_t __cdecl ApiRevision() noexcept { return g_revision.load(); }
int __cdecl ApiAddListener(livetables::ChangeFn fn, void* user) noexcept {
	if (fn == nullptr) return 0;
	AcquireSRWLockExclusive(&g_listenLock);
	const int h = g_nextHandle++;
	g_listeners.push_back({ h, fn, user });
	ReleaseSRWLockExclusive(&g_listenLock);
	return h;
}
void __cdecl ApiRemoveListener(int handle) noexcept {
	AcquireSRWLockExclusive(&g_listenLock);
	for (size_t i = 0; i < g_listeners.size(); ++i) if (g_listeners[i].handle == handle) { g_listeners.erase(g_listeners.begin() + static_cast<long>(i)); break; }
	ReleaseSRWLockExclusive(&g_listenLock);
}

int Queue(Request* q) {
	AcquireSRWLockExclusive(&g_lock);
	if (!g_link.ok || g_threads == nullptr || !OkTable(static_cast<livetables::Table>(q->table))) {
		q->why = !g_link.ok ? "Live Tables is off: " + g_linkProblem : "no such table";
		g_finished.push_back(q);
		ReleaseSRWLockExclusive(&g_lock);
		if (g_threads != nullptr) PostDone(); else DoneJob(g_ctx, nullptr);
		return 1;
	}
	g_queue.push_back(q);
	ReleaseSRWLockExclusive(&g_lock);
	Start();
	return 1;
}
// Remove vetoes: other plugins' own files (recipes.txt ...) that the read hook never sees. Asked on
// the caller's thread before anything is queued; Live Tables holds no lock of its own then.


void __cdecl HolderJob(const D2RL::PluginContext*, void* user) noexcept {
	SeenUi();
	Request* q = static_cast<Request*>(user);
	q->loadedHolders = q->number == SIZE_MAX ? -1 : LoadedHolders(q->number);
	Queue(q);
}

int RowRequest(Request::Kind kind, livetables::Table table, int dataVersion, int number, livetables::DoneFn done, void* user) {
	if (done == nullptr) return 0;
	auto* q = new Request { static_cast<lt::TableId>(table), dataVersion, "", done, user };
	q->kind = kind;
	q->number = number < 0 ? SIZE_MAX : static_cast<size_t>(number);
	if (kind == Request::Remove) {
		AcquireSRWLockShared(&g_vetoLock);
		const std::vector<Veto> list = g_vetoes;
		ReleaseSRWLockShared(&g_vetoLock);
		for (const Veto& v : list) {
			char why[512] = "";
			if (v.fn(v.user, table, dataVersion, number, why, sizeof(why)) == 0) {
				q->why = why[0] != '\0' ? why : "another plugin keeps a file that names it";
				AcquireSRWLockExclusive(&g_lock);
				g_finished.push_back(q);
				ReleaseSRWLockExclusive(&g_lock);
				if (g_threads != nullptr) PostDone(); else DoneJob(g_ctx, nullptr);
				return 1;
			}
		}
	}
	if (kind == Request::Remove && static_cast<lt::TableId>(table) == lt::TableId::ItemStatCost && g_threads != nullptr
		&& g_threads->runOnUiThread(g_ctx, HolderJob, q) == D2RL::Threads::Result::Success) return 1; // HolderJob queues it
	return Queue(q);
}
int __cdecl ApiAddVeto(livetables::CanRemoveFn fn, void* user) noexcept {
	if (fn == nullptr) return 0;
	AcquireSRWLockExclusive(&g_vetoLock);
	const int h = g_nextVeto++;
	g_vetoes.push_back({ h, fn, user });
	ReleaseSRWLockExclusive(&g_vetoLock);
	return h;
}
void __cdecl ApiRemoveVeto(int handle) noexcept {
	AcquireSRWLockExclusive(&g_vetoLock);
	g_vetoes.erase(std::remove_if(g_vetoes.begin(), g_vetoes.end(), [&](const Veto& v) { return v.handle == handle; }), g_vetoes.end());
	ReleaseSRWLockExclusive(&g_vetoLock);
}
int __cdecl ApiDisable(livetables::Table t, int dv, int number, livetables::DoneFn done, void* user) noexcept { return RowRequest(Request::Disable, t, dv, number, done, user); }
int __cdecl ApiEnable(livetables::Table t, int dv, int number, livetables::DoneFn done, void* user) noexcept { return RowRequest(Request::Enable, t, dv, number, done, user); }
int __cdecl ApiRemove(livetables::Table t, int dv, int number, livetables::DoneFn done, void* user) noexcept { return RowRequest(Request::Remove, t, dv, number, done, user); }
int __cdecl ApiRename(livetables::Table t, int dv, int number, const char* newName, livetables::DoneFn done, void* user) noexcept {
	if (done == nullptr) return 0;
	auto* q = new Request { static_cast<lt::TableId>(t), dv, newName != nullptr ? newName : "", done, user };
	q->kind = Request::Rename;
	q->number = number < 0 ? SIZE_MAX : static_cast<size_t>(number);
	return Queue(q);
}
int __cdecl ApiSetCells(livetables::Table t, int dv, int number, const char* cells, livetables::DoneFn done, void* user) noexcept {
	if (done == nullptr) return 0;
	auto* q = new Request { static_cast<lt::TableId>(t), dv, cells != nullptr ? cells : "", done, user };
	q->kind = Request::SetCells;
	q->number = number < 0 ? SIZE_MAX : static_cast<size_t>(number);
	return Queue(q);
}
int __cdecl ApiMoved(livetables::Table t, int dv, livetables::MovedRow* out, size_t room) noexcept {
	AcquireSRWLockShared(&g_lock);
	int n = 0;
	for (const lt::Moved& m : g_movedLast) {
		const int v = m.which == 0 ? 3 : 1;
		if (static_cast<livetables::Table>(m.table) != t || (dv != 0 && dv != v && !(v == 1 && dv == 2))) continue;
		if (out != nullptr && static_cast<size_t>(n) < room) {
			livetables::MovedRow& r = out[n];
			memset(&r, 0, sizeof(r));
			snprintf(r.name, sizeof(r.name), "%s", m.name.c_str());
			r.was = static_cast<int32_t>(m.was); r.now = static_cast<int32_t>(m.now); r.dataVersion = v;
		}
		++n;
	}
	ReleaseSRWLockShared(&g_lock);
	return n;
}
int __cdecl ApiFixMoved(livetables::DoneFn done, void* user) noexcept { return RowRequest(Request::FixMoved, livetables::Table::ItemStatCost, 0, 0, done, user); }
int __cdecl ApiStatus(char* text, size_t size) noexcept {
	AcquireSRWLockShared(&g_lock);
	const std::string a = g_alarm;
	ReleaseSRWLockShared(&g_lock);
	if (text != nullptr && size != 0) snprintf(text, size, "%s", a.c_str());
	return a.empty() ? 0 : 1;
}

// list(): grown-rows.txt and disabled-rows.txt as Live Tables holds them, copied under the lock,
// handed out after it.
int __cdecl ApiList(livetables::Table t, int dv, livetables::KeptFn callback, void* user) noexcept {
	if (callback == nullptr) return 0;
	struct Row { std::string name, cells, versions; int32_t number; int32_t disabled; };
	std::vector<Row> rows;
	const char want = dv == 0 ? 0 : static_cast<char>('0' + dv);
	AcquireSRWLockExclusive(&g_lock);
	ReadKeptOnce();
	std::string off;
	ReadWhole(OffPath(), off);
	std::set<std::string> offNames; // "name|versions"
	for (size_t at = 0; at < off.size();) {
		size_t end = off.find('\n', at); if (end == std::string::npos) end = off.size();
		std::string l = off.substr(at, end - at); at = end + 1;
		if (!l.empty() && l.back() == '\r') l.pop_back();
		if (l.empty() || l[0] == '#') continue;
		const size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
		if (b == std::string::npos || l.compare(0, a, "itemstatcost") != 0 || l.compare(a + 1, 2, "v=") != 0) continue;
		const std::string versions = l.substr(a + 3, b - a - 3), name = lt::Lower(l.substr(b + 1));
		if (want == 0 || versions.find(want) != std::string::npos) offNames.insert(name);
	}
	for (const lt::KeptRow& k : g_kept) {
		if (static_cast<livetables::Table>(k.table) != t || (want != 0 && k.versions.find(want) == std::string::npos)) continue;
		Row r;
		r.name = k.name;
		r.versions = k.versions;
		for (const auto& [c, v] : k.cells) r.cells += (r.cells.empty() ? "" : "\t") + c + "=" + v;
		const size_t n = dv == 0 ? k.number : lt::KeptNumberFor(k, dv == 3 ? 0 : 1);
		r.number = n == SIZE_MAX ? -1 : static_cast<int32_t>(n);
		r.disabled = k.table == lt::TableId::ItemStatCost && offNames.count(lt::Lower(k.name)) ? 1 : 0;
		rows.push_back(std::move(r));
	}
	ReleaseSRWLockExclusive(&g_lock);
	for (const Row& r : rows) {
		livetables::KeptRowInfo info { sizeof(livetables::KeptRowInfo), r.number, r.name.c_str(), r.cells.c_str(), r.versions.c_str(), r.disabled };
		callback(user, t, &info);
	}
	return static_cast<int>(rows.size());
}

int __cdecl ApiIsDisabled(livetables::Table t, int dv, int number) noexcept {
	if (t != livetables::Table::ItemStatCost || number < 0) return 0;
	AcquireSRWLockShared(&g_lock);
	const int r = g_off.count({ OffKey(OneVersion(dv)), static_cast<size_t>(number) }) != 0 ? 1 : 0;
	ReleaseSRWLockShared(&g_lock);
	return r;
}

const livetables::Api g_api { sizeof(livetables::Api), livetables::kApiVersion, ApiCheck, ApiGrow, ApiCount, ApiLimit, ApiFind, ApiRevision, ApiAddListener, ApiRemoveListener,
                              ApiDisable, ApiEnable, ApiIsDisabled, ApiRemove, ApiRename, ApiAddVeto, ApiRemoveVeto, ApiStatus, ApiFixMoved, ApiMoved, ApiSetCells, ApiList };

// ---------------------------------------------------------------- events, console

// The moved kept rows of this table read: properties follow (their kept number is updated), stats
// raise the alarm. Runs even when growing is off (the hook still lays the rows in).
void MovedAtLoad() {
	AcquireSRWLockExclusive(&g_lock);
	std::string alarm;
	bool keptChanged = false;
	for (const lt::Moved& m : g_moved) {
		if (m.table == lt::TableId::Properties) {
			for (lt::KeptRow& k : g_kept) {
				if (k.table != m.table || lt::Lower(k.name) != lt::Lower(m.name)) continue;
				const bool in3 = k.versions.find('3') != std::string::npos;
				if (m.which == 0) k.number = m.now;
				else if (k.numberBase != SIZE_MAX) k.numberBase = m.now;
				else if (!in3) k.number = m.now;
				keptChanged = true;
			}
			LogF("table rows: property %s is number %zu now (was %zu, %s): no save holds property numbers; its kept number follows", m.name.c_str(), m.now, m.was, m.which == 0 ? "excel" : "excel\\base");
			continue;
		}
		alarm += (alarm.empty() ? "" : "; ") + m.name + " " + std::to_string(m.was) + " -> " + std::to_string(m.now) + (m.which == 0 ? " (data version 3)" : " (data versions 1/2)");
	}
	if (keptChanged && !WriteKept()) LogF("WARNING: grown-rows.txt could not be written");
	g_movedStats.clear();
	for (const lt::Moved& m : g_moved) if (m.table == lt::TableId::ItemStatCost) g_movedStats.push_back(m);
	g_movedLast = g_moved;
	g_startTime = static_cast<int64_t>(time(nullptr));
	g_moved.clear();
	g_alarm = alarm.empty() ? std::string()
	        : "made stats moved at this start: " + alarm + ". Saved items that hold them read as other stats now: do not load such characters. Stat changes are refused until the saves are renumbered at the menu (tables fixmoved)";
	ReleaseSRWLockExclusive(&g_lock);
	if (!g_alarm.empty()) {
		LogF("ALARM: %s", g_alarm.c_str());
		livetables::Change ch {};
		ch.size = sizeof(ch); ch.kind = livetables::ChangeKind::Alarm; ch.table = livetables::Table::ItemStatCost; ch.revision = ++g_revision;
		Tell(ch);
	}
}

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	MovedAtLoad();
	RecoverJournal();
	if (!g_link.ok) return;
	AcquireSRWLockExclusive(&g_lock);
	const std::vector<Retired> old = std::move(g_retired);
	g_retired.clear();
	g_maps.clear();
	g_renameNames.clear();
	ReapplyOff();
	ReleaseSRWLockExclusive(&g_lock);
	size_t freed = 0;
	for (const Retired& r : old) if (r.gameOwned && r.rows != nullptr) { GameFree(r.rows); ++freed; }
	const uint64_t revision = ++g_revision;
	for (int v = 1; v <= 3; ++v) {
		if (!BankUp(v)) continue;
		for (int t = 0; t < 2; ++t) {
			const Vector* vec = VectorOf(static_cast<lt::TableId>(t), v);
			livetables::Change ch { sizeof(livetables::Change), livetables::ChangeKind::Load, static_cast<livetables::Table>(t), v, nullptr, vec->rows, 0, vec->count, lt::RowSizeOf(static_cast<lt::TableId>(t)), revision };
			Tell(ch);
		}
		LogF("tables read: data version %d: %llu of %zu stats, %llu of %zu properties", v,
		     static_cast<unsigned long long>(VectorOf(lt::TableId::ItemStatCost, v)->count), LimitOf(lt::TableId::ItemStatCost),
		     static_cast<unsigned long long>(VectorOf(lt::TableId::Properties, v)->count), LimitOf(lt::TableId::Properties));
	}
	if (freed != 0) LogF("%zu old row buffers freed (the tables were read again)", freed);
	if (g_threads != nullptr) static_cast<void>(g_threads->runOnUiThread(g_ctx, ProbeJob, nullptr)); // learn the UI thread before any grow
}

void __cdecl ConsoleDone(void*, const livetables::GrowDone* d) noexcept {
	char text[600];
	if (d->ok) snprintf(text, sizeof(text), "Live Tables: done - row %d (data version 3), %d (1/2); %u older rows re-linked. %s", d->numbers[3], d->numbers[1] >= 0 ? d->numbers[1] : d->numbers[2], d->relinked, d->why);
	else snprintf(text, sizeof(text), "Live Tables: not done - %s", d->why);
	g_ctx->WriteConsoleMessage(text);
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	if (!g_link.ok) {
		g_ctx->WriteConsoleMessage(("Live Tables: off - " + g_linkProblem).c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	char what[16] = "", which[16] = "";
	int  used = 0;
	if (sscanf(args.c_str(), " %15s %15s %n", what, which, &used) >= 2 && strcmp(what, "grow") == 0) {
		const livetables::Table t = strcmp(which, "stat") == 0 ? livetables::Table::ItemStatCost : strcmp(which, "prop") == 0 ? livetables::Table::Properties : static_cast<livetables::Table>(-1);
		if (!OkTable(t)) { g_ctx->WriteConsoleMessage("Live Tables: tables grow stat|prop column=value;column=value..."); return D2RL::ConsoleCommandResult::Handled; }
		std::string row = args.substr(static_cast<size_t>(used));
		for (char& c : row) if (c == ';') c = '\t';
		ApiGrow(t, 0, row.c_str(), ConsoleDone, nullptr);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (sscanf(args.c_str(), " %15s", what) == 1 && !strcmp(what, "fixmoved")) { ApiFixMoved(ConsoleDone, nullptr); return D2RL::ConsoleCommandResult::Handled; }
	if (sscanf(args.c_str(), " %15s", what) == 1 && !strcmp(what, "status")) {
		char text[1024];
		g_ctx->WriteConsoleMessage(ApiStatus(text, sizeof(text)) ? (std::string("Live Tables ALARM: ") + text).c_str() : "Live Tables: no alarm");
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (sscanf(args.c_str(), " %15s %15s %n", what, which, &used) >= 2 && !strcmp(what, "rename") && (!strcmp(which, "stat") || !strcmp(which, "prop"))) {
		char was[64] = "", now[64] = "";
		if (sscanf(args.c_str() + used, " %63s %63s", was, now) != 2) { g_ctx->WriteConsoleMessage("Live Tables: tables rename stat|prop <old name> <new name>"); return D2RL::ConsoleCommandResult::Handled; }
		const livetables::Table t = !strcmp(which, "stat") ? livetables::Table::ItemStatCost : livetables::Table::Properties;
		const int n = ApiFind(t, 0, was);
		if (n < 0) { g_ctx->WriteConsoleMessage((std::string("Live Tables: no row named ") + was).c_str()); return D2RL::ConsoleCommandResult::Handled; }
		ApiRename(t, 0, n, now, ConsoleDone, nullptr);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (sscanf(args.c_str(), " %15s %15s %n", what, which, &used) >= 2 && !strcmp(what, "remove") && !strcmp(which, "prop")) {
		std::string name = args.substr(static_cast<size_t>(used));
		while (!name.empty() && name.back() == ' ') name.pop_back();
		const int n = ApiFind(livetables::Table::Properties, 0, name.c_str());
		if (n < 0) { g_ctx->WriteConsoleMessage(("Live Tables: no property code " + name).c_str()); return D2RL::ConsoleCommandResult::Handled; }
		RowRequest(Request::Remove, livetables::Table::Properties, 0, n, ConsoleDone, nullptr);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (sscanf(args.c_str(), " %15s %15s %n", what, which, &used) >= 2 && (!strcmp(what, "disable") || !strcmp(what, "enable") || !strcmp(what, "remove")) && !strcmp(which, "stat")) {
		std::string name = args.substr(static_cast<size_t>(used));
		while (!name.empty() && name.back() == ' ') name.pop_back();
		const int n = ApiFind(livetables::Table::ItemStatCost, 0, name.c_str());
		if (n < 0) { g_ctx->WriteConsoleMessage(("Live Tables: no stat named " + name).c_str()); return D2RL::ConsoleCommandResult::Handled; }
		const Request::Kind k = !strcmp(what, "disable") ? Request::Disable : !strcmp(what, "enable") ? Request::Enable : Request::Remove;
		RowRequest(k, livetables::Table::ItemStatCost, 0, n, ConsoleDone, nullptr);
		return D2RL::ConsoleCommandResult::Handled;
	}
	char text[400];
	for (int v = 3; v >= 1; --v) {
		if (!BankUp(v)) continue;
		snprintf(text, sizeof(text), "Live Tables %s, data version %d: %d of %d stats used, %d of %d properties used; table rows: %s", kVersion, v,
		         ApiCount(livetables::Table::ItemStatCost, v), ApiLimit(livetables::Table::ItemStatCost, v), ApiCount(livetables::Table::Properties, v),
		         ApiLimit(livetables::Table::Properties, v), g_tableHook ? "on" : ("OFF (" + g_hookProblem + ")").c_str());
		g_ctx->WriteConsoleMessage(text);
	}
	g_ctx->WriteConsoleMessage("Live Tables: tables grow stat|prop column=value;column=value...  (e.g. tables grow stat stat=my_stat;saved=1;save bits=8)");
	g_ctx->WriteConsoleMessage("Live Tables: tables disable|enable|remove stat <name> | remove prop <code> | rename stat|prop <old> <new>");
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "live-tables",
	.name        = "Live Tables",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "ItemStatCost and Properties grow by a row while the game runs, with the game's own routines.",
	.flags       = static_cast<D2RL::PluginFlags>(static_cast<uint32_t>(D2RL::PluginFlags::Shared) | static_cast<uint32_t>(D2RL::PluginFlags::NativeHooks)),
	.reserved    = {},
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* { return &kPluginInfo; }

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) return false;
	g_ctx = context;
	LoadConfig();
	LogF("build: Live Tables %s, d2rl-live-tables.dll sha256 %s, API version %u", kVersion, OwnSha256().c_str(), livetables::kApiVersion);
	static_cast<void>(context->RegisterConsoleCommand("tables", OnConsole, "Live Tables: rows used / room in ItemStatCost and Properties; tables grow ..."));
	if (!g_enabled) { LogF("Live Tables %s: off in the settings (enabled = false)", kVersion); return true; }
	// The read hook first, on its own: it needs only D2RCore's read and vector routines (their bytes
	// are checked), so rows made earlier still reach the game after a game or loader update, even
	// when growing is off (characters that hold made stats depend on it).
	g_core = GetModuleHandleW(L"D2RCore.dll");
	const std::wstring props = LoaderDir() + L"\\plugins\\d2rl-live-properties.dll";
	if (g_core == nullptr) {
		g_hookProblem = "D2RCore.dll is not loaded";
		LogF("table rows: OFF - %s. Made rows do NOT reach the game this start.", g_hookProblem.c_str());
	} else if (GetFileAttributesW(props.c_str()) != INVALID_FILE_ATTRIBUTES && !FileExports(props, "LivePropsUsesLiveTables")) {
		// Only one plugin may add rows to these tables as the game reads them. Live Properties up to
		// 0.9.x does it itself (its own hook on the same routine): then Live Tables leaves them alone.
		g_hookProblem = "Live Properties 0.9.x is installed and adds its own rows to itemstatcost.txt / properties.txt; Live Tables leaves these tables alone until Live Properties 0.10";
		LogF("table rows: OFF - %s. Nothing is grown.", g_hookProblem.c_str());
	} else if (InstallTableHook(g_hookProblem)) {
		g_tableHook = true;
		LogF("table rows: on (rows grown during play are added to itemstatcost.txt / properties.txt as the game reads them, from %ls)", KeptPath().c_str());
	} else {
		LogF("table rows: OFF - %s. Made rows do NOT reach the game this start.", g_hookProblem.c_str());
	}
	if (!LinkGame()) {
		LogF("Live Tables %s: growing, disabling, removing and renaming are OFF - %s%s", kVersion, g_linkProblem.c_str(),
		     g_tableHook ? " (the table rows above still reach the game)" : "");
	} else if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
		g_link.ok = false;
		g_linkProblem = "this D2RLoader has no thread service";
		LogF("Live Tables %s: growing etc. OFF - %s", kVersion, g_linkProblem.c_str());
	}
	g_timerQueue = CreateTimerQueue();
	if (context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success) g_items = nullptr;
	if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success) g_inventory = nullptr;
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, registerGameplayEventListener))) {
		D2RL::Lifecycle::DataTablesLoadedListener tables {};
		tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
		tables.callback   = OnTablesLoaded;
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
	}
	HMODULE self = nullptr; // stay loaded: the game's tables point into memory grown here and our hook
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&GameJob), &self);
	if (g_link.ok) LogF("Live Tables %s: on - type tables in the console (D2R %s)", kVersion, context->buildVersion != nullptr ? context->buildVersion : "?");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}

// The suite's rule: the newest Api for any caller asking version >= 1; the caller checks
// api->version and api->size for the fields it needs.
D2RL_PLUGIN_EXPORT const livetables::Api* __cdecl LiveTablesGetApi(uint32_t version) noexcept {
	return version >= 1 ? &g_api : nullptr;
}
