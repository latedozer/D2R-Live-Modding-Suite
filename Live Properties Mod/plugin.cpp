// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties: the game's item tables changed while a game runs, in the game's memory. No
// table reload, no restart, the mod's own files are never written.
//
// Tabs in the Mod Book:
//   Properties  every row of the property table; a row's card changes its func / stat / set / val
//   Stats       every stat; a card changes a row, the op linking done the game's own way
//               (prop_core.h). New (and New property) asks Live Tables for a row: it is added at
//               once, during play, and Live Tables keeps it (live_tables_api.h). A made stat is
//               kept by its name. Without Live Tables the tabs change the rows that are there.
//   Funcs       the property funcs the game has
//   Affixes     every magic prefix / suffix (affix_core.h); New makes one of an empty row
// Each tab's buttons stand on the Mod Book's action row (Mod Book 0.8.0). The Legend (Mod Book's,
// right side) gets three sections from here: Properties, Stats, Affixes (legend_core.h), with
// examples from the tables the game read.
// The changes are kept in d2rloader\live-suite\live-properties\<mod>\ (property-edits.txt,
// stat-edits.txt, affix-edits.txt) and put back every time the game has read its tables.
//
// Console (Ctrl + `): prop | prop book | prop stats | prop funcs | prop affixes
//

#include <D2RLPlugin/api.h>

#include "prop_core.h"

#include "../Mod Book/mod_book_api.h"
#include "../Save Scan/save_scan_api.h"
#include "live_props_api.h"
#include "lt_core.h"
#include "card_core.h"
#include "op_core.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <deque>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "affix_core.h"
#include "legend_core.h"
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.12.0";
// Stats > New and New property: a row is added at once, during play, or not at all (user 2026-11-25).
constexpr const char* kNeedsLiveTables = "New needs Live Tables (adds the row at once, during play).";
constexpr size_t      kStatMost        = 510; // items save stat numbers in 9 bits

const D2RL::PluginContext* g_ctx = nullptr;

void LogF(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

// ---------------------------------------------------------------- settings

bool g_enabled = true;

void LoadConfig() {
	static char text[4096];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		return;
	}
	for (const char* line = text; line != nullptr && *line != '\0';) {
		const char* end = strchr(line, '\n');
		char        buffer[256], key[32], value[32];
		snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(end != nullptr ? end - line : static_cast<ptrdiff_t>(strlen(line))), line);
		line = end != nullptr ? end + 1 : nullptr;
		if (sscanf(buffer, " %31[a-z_] = %31s", key, value) == 2 && strcmp(key, "enabled") == 0) {
			g_enabled = strncmp(value, "true", 4) == 0 || *value == '1';
		}
	}
}

// ---------------------------------------------------------------- the game

constexpr uint32_t kKnownStamp = 0x6ab3782c; // the D2R image all addresses below are from

constexpr uintptr_t kTablesRva  = 0x300a90; // data tables of a data version (1 classic, 2 LoD, 3 RotW)
constexpr uintptr_t kLookupRva  = 0xa121f0; // name -> row number in one of the game's name tables; below 0 = no such name
constexpr uintptr_t kStatNamesAt = 0x1270;  // the stat name table in the data tables (read by the cube's "param" column, 0x3e5dd0)
constexpr uintptr_t kStatsAt    = 0x1258;   // the ItemStatCost vector in the data tables
constexpr size_t    kAdvDisplayAt = 0x140;  // ItemStatCost row: the advdisplay byte (0 never, 1 always, 2 non-zero; Live Tables' column map)
constexpr uintptr_t kPropNamesAt = 0x270;   // the property code table in the data tables (read by the cube's "mod" columns, 0x32042c)
constexpr uintptr_t kPropsAt    = 0x240;    // the Properties vector in the data tables
constexpr uintptr_t kAffixesAt  = 0x15e8;   // the magic affix vector: suffixes, then prefixes, then automagic (loader 0x3195c0)
constexpr uintptr_t kAffixPrefixAt = 0x1608, kAffixAutoAt = 0x1610; // where its prefixes and its automagic rows start (pointers)
constexpr uintptr_t kItemTypesAt = 0x1348;  // the ItemTypes vector (rows of 0xe8, the 4-letter code first; 0x318e40)
constexpr size_t    kItemTypeRow = 0xe8;
constexpr uintptr_t kOpFlagsRva = 0x1d996e0;
constexpr uintptr_t kFuncCountRva = 0x2386bd8; // how many property funcs the game has (read at 0x3d4715)
constexpr uintptr_t kFuncTableRva = 0x2386ab0; // their handlers, one pointer each; 0 = the func does nothing (0x3d46fb)

// A vector of the game. A capacity with the top bit set means the memory is not the vector's:
// the game then neither frees nor grows it (for the property vector: 0x2fd1dc).
struct Vector {
	uint8_t* rows;
	uint64_t count;
	uint64_t capacity;
};
constexpr uint64_t kNotOwned = 0x8000000000000000ull;

struct Link {
	bool ok = false;
	void* (*tables)(uint8_t version)                                  = nullptr;
	int (*lookup)(void* table, const char* text, int unused)          = nullptr;
	// D2RCore's exports (found by name; each may be missing in another D2RLoader)
	void (*addStat)(void* unit, uint32_t stat, int32_t value, uint32_t layer) = nullptr; // AddWideUnitStat
	int32_t (*readStat)(void* unit, uint32_t stat, uint32_t layer)             = nullptr; // ReadWideUnitStat (D2RCore 0x3d8bd0: unit, stat < 0x8000, layer; reads only)
	void (*descOrder)(void* tables)                                           = nullptr; // BuildItemStatDescriptionOrder
	uint16_t (*stringId)(const void* keyView)                                 = nullptr; // GetNamespacedStringId({text, length})
	const char* (*stringById)(uint32_t id)                                    = nullptr; // GetNamespacedStringById
};
Link        g_link;
std::string g_linkProblem;
uintptr_t   g_imageBase = 0;

uint32_t ImageStamp(uintptr_t base) {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (base == 0 || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xE00) {
		return 0;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

struct Print {
	uintptr_t   rva;
	const char* what;
	bool        entry; // the start of a function another plugin may have hooked (then it starts with a jump)
	uint8_t     size;
	uint8_t     bytes[16];
};
constexpr Print kPrints[] = {
	{ kTablesRva, "the data tables getter", true, 16, { 0x48, 0x83, 0xec, 0x28, 0x0f, 0xb6, 0xc1, 0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0x83, 0xf8, 0x04 } },
	{ kLookupRva, "the name table lookup", true, 16, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x05 } },
	{ 0x3e5dd0, "the stat name table's place", false, 7, { 0x48, 0x8b, 0x88, 0x70, 0x12, 0x00, 0x00 } },
	{ 0x31ecd5, "the stat table's place", false, 7, { 0x48, 0x8d, 0xbe, 0x58, 0x12, 0x00, 0x00 } },
	{ 0x31ed76, "the stat row size", false, 9, { 0x48, 0x69, 0x44, 0x24, 0x50, 0x44, 0x01, 0x00, 0x00 } },
	{ 0x31ef37, "the dependent list's place", false, 6, { 0x66, 0x41, 0x89, 0x74, 0x48, 0x5a } },
	{ 0x31effb, "the op list's place", false, 7, { 0x66, 0x89, 0xb2, 0xdc, 0x00, 0x00, 0x00 } },
	{ 0x32042c, "the property code table's place", false, 7, { 0x48, 0x8b, 0x88, 0x70, 0x02, 0x00, 0x00 } },
	{ 0x3d4715, "the property func count's place", false, 6, { 0x3b, 0x0d, 0xbd, 0x24, 0xfb, 0x01 } },
	{ 0x3d46fb, "the property func table's place", false, 7, { 0x48, 0x8d, 0x15, 0xae, 0x23, 0xfb, 0x01 } },
	{ 0x3d326d, "the property table's place", false, 7, { 0x48, 0x81, 0xc7, 0x40, 0x02, 0x00, 0x00 } },
	{ 0x319e4c, "the affix table's place", false, 7, { 0x49, 0x8d, 0x8e, 0xe8, 0x15, 0x00, 0x00 } },
	{ 0x319e9f, "the affix table's prefix start", false, 7, { 0x49, 0x89, 0x8e, 0x08, 0x16, 0x00, 0x00 } },
	{ 0x319ec7, "the affix table's automagic start", false, 7, { 0x49, 0x89, 0x8e, 0x10, 0x16, 0x00, 0x00 } },
	{ 0x319f1d, "the affix row size", false, 11, { 0x49, 0x69, 0xbe, 0xf0, 0x15, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00 } },
	{ 0x319f6e, "the affix name's string number", false, 4, { 0x66, 0x89, 0x43, 0x20 } },
	{ 0x318e40, "the item type table's place", false, 7, { 0x48, 0x8d, 0xbe, 0x48, 0x13, 0x00, 0x00 } },
	{ 0x318eab, "the item type row size", false, 8, { 0x48, 0x69, 0x7f, 0x08, 0xe8, 0x00, 0x00, 0x00 } },
	{ 0x97cc2, "the client unit table's place", false, 7, { 0x4c, 0x8d, 0x05, 0x47, 0xbc, 0x98, 0x02 } },
	{ kOpFlagsRva, "the op flag values", false, 16, { 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00 } },
};
bool g_printsChecked = false;

bool LinkGame() {
	g_imageBase          = g_ctx->exeBase != 0 ? g_ctx->exeBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const uint32_t stamp = ImageStamp(g_imageBase);
	if (stamp != kKnownStamp) {
		char text[200];
		snprintf(text, sizeof(text), "this is another build of the game (image stamp %08x; the plugin knows %08x)", stamp, kKnownStamp);
		g_linkProblem = text;
		return false;
	}
	g_link.tables = reinterpret_cast<decltype(g_link.tables)>(g_imageBase + kTablesRva);
	g_link.lookup = reinterpret_cast<decltype(g_link.lookup)>(g_imageBase + kLookupRva);
	if (const HMODULE core = GetModuleHandleW(L"D2RCore.dll"); core != nullptr) {
		g_link.addStat    = reinterpret_cast<decltype(g_link.addStat)>(reinterpret_cast<void*>(GetProcAddress(core, "AddWideUnitStat")));
		g_link.readStat   = reinterpret_cast<decltype(g_link.readStat)>(reinterpret_cast<void*>(GetProcAddress(core, "ReadWideUnitStat")));
		g_link.descOrder  = reinterpret_cast<decltype(g_link.descOrder)>(reinterpret_cast<void*>(GetProcAddress(core, "BuildItemStatDescriptionOrder")));
		g_link.stringId   = reinterpret_cast<decltype(g_link.stringId)>(reinterpret_cast<void*>(GetProcAddress(core, "GetNamespacedStringId")));
		g_link.stringById = reinterpret_cast<decltype(g_link.stringById)>(reinterpret_cast<void*>(GetProcAddress(core, "GetNamespacedStringById")));
	}
	g_link.ok = true;
	return true;
}

// Once the game has read its tables: is the code the plugin relies on what it was written against?
bool CheckPrints() {
	if (g_printsChecked) {
		return g_link.ok;
	}
	g_printsChecked = true;
	for (const Print& print : kPrints) {
		const auto* at = reinterpret_cast<const uint8_t*>(g_imageBase + print.rva);
		if (memcmp(at, print.bytes, print.size) == 0) {
			continue;
		}
		if (print.entry && (at[0] == 0xe9 || (at[0] == 0xff && at[1] == 0x25))) {
			LogF("note: %s starts with a jump (something has hooked it); it is called through that hook", print.what);
			continue;
		}
		g_linkProblem = std::string("the game's code is not what the plugin expects at ") + print.what;
		g_link.ok     = false;
		LogF("off: %s. Nothing is changed.", g_linkProblem.c_str());
		return false;
	}
	return true;
}

bool Readable(const void* address, size_t size) {
	MEMORY_BASIC_INFORMATION info {};
	if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	return static_cast<const uint8_t*>(address) + size <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

// ---------------------------------------------------------------- text and files

std::wstring Widen(const std::string& text) {
	std::wstring out(text.size(), L'\0');
	const int    n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
	out.resize(n > 0 ? static_cast<size_t>(n) : 0);
	return out;
}

std::string Narrow(const std::wstring& text) {
	std::string out(text.size() * 3, '\0');
	const int   n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
	out.resize(n > 0 ? static_cast<size_t>(n) : 0);
	return out;
}

std::string g_moved; // set when the folder was moved from the old place (logged at start)
// d2rloader\live-suite\<plugin>\<mod> (every plugin of the Live Suite keeps its files under
// live-suite). A folder in the old place, d2rloader\<plugin>\<mod>, is moved there once.
std::wstring g_loaderDir; // ...\d2rloader
std::string  g_modName;   // the active mod ("D2RMM")

std::wstring SuiteFolder(const std::wstring& loader, const wchar_t* plugin, const std::wstring& mod, std::string& moved) {
	const std::wstring now = loader + L"\\live-suite\\" + plugin + L"\\" + mod, old = loader + L"\\" + plugin + L"\\" + mod;
	const DWORD        was = GetFileAttributesW(old.c_str());
	if (GetFileAttributesW(now.c_str()) == INVALID_FILE_ATTRIBUTES && was != INVALID_FILE_ATTRIBUTES && (was & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		CreateDirectoryW((loader + L"\\live-suite").c_str(), nullptr);
		CreateDirectoryW((loader + L"\\live-suite\\" + plugin).c_str(), nullptr);
		if (MoveFileExW(old.c_str(), now.c_str(), 0)) {
			RemoveDirectoryW((loader + L"\\" + plugin).c_str()); // only goes when empty
			moved = "moved from the old place, d2rloader\\" + std::string(plugin, plugin + wcslen(plugin)) + ", to d2rloader\\live-suite";
		} else {
			moved = "could not be moved from the old place (error " + std::to_string(GetLastError()) + "); a new folder is used";
		}
	}
	return now;
}

// <loader folder>\live-suite\live-properties\<mod>: beside the plugins folder. Never inside a mod's .mpq.
std::wstring OwnFolder() {
	std::wstring loader;
	if (g_ctx->pluginDirectory != nullptr && g_ctx->pluginDirectory[0] != L'\0') {
		loader = g_ctx->pluginDirectory;
		while (!loader.empty() && (loader.back() == L'\\' || loader.back() == L'/')) {
			loader.pop_back();
		}
		const size_t slash = loader.find_last_of(L"\\/");
		loader             = slash != std::wstring::npos && _wcsicmp(loader.c_str() + slash + 1, L"plugins") == 0 ? loader.substr(0, slash) : std::wstring();
	}
	if (loader.empty()) {
		wchar_t     path[1024] = {};
		const DWORD length     = GetModuleFileNameW(nullptr, path, 1000);
		loader.assign(path, length < 1000 ? length : 0);
		const size_t slash = loader.find_last_of(L'\\');
		loader             = (slash != std::wstring::npos ? loader.substr(0, slash) : loader) + L"\\d2rloader";
	}
	const std::string mod = g_ctx->activeMod != nullptr && g_ctx->activeMod[0] != '\0' ? g_ctx->activeMod : "no mod";
	g_loaderDir           = loader;
	g_modName             = mod;
	return SuiteFolder(loader, L"live-properties", Widen(mod), g_moved);
}

void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	text.clear();
	char  buffer[4096];
	DWORD got = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got != 0) {
		text.append(buffer, got);
	}
	CloseHandle(file);
	return true;
}

// Writes (or replaces) a file the plugin itself makes: first beside it, then moved into place.
bool WriteOwn(const std::wstring& path, const std::string& text) {
	const std::wstring temp = path + L".writing";
	const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD      written = 0;
	const bool ok      = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
	CloseHandle(file);
	return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------- state

struct Bank { // one data version's tables
	uint8_t     version = 0;
	bool        on      = false;
	void*       tables  = nullptr;
	std::string off;
	Vector*     stats    = nullptr;
	Vector*     props    = nullptr;
	uint8_t*    propRows = nullptr; // the plugin's own copy of the property table, with the extra rows
	uint64_t    propBase = 0;       // the game's own property count = the number of property 1
	std::vector<uint8_t>     propOrig;              // the game's own rows as it read them
	std::vector<uint8_t>     statOrig;              // the same for the stat table
	const uint8_t*           statOrigOf = nullptr;  // the rows statOrig was taken from
	std::vector<std::string> statNames, propNames; // from the game's own name tables; "" = no name
	// the magic affixes (the Affixes tab)
	bool                     affixOn = false;
	Vector*                  affixes = nullptr;
	size_t                   preBase = 0, autoBase = 0; // where the prefixes and the automagic rows start in it
	std::vector<uint8_t>     affixOrig;                  // the rows as the game read them
	const uint8_t*           affixOrigOf = nullptr;
	std::vector<std::string> typeCodes;                  // item type codes by row
	affix::Names             names;
};

SRWLOCK                  g_lock = SRWLOCK_INIT; // everything below
Bank                     g_banks[3];
bool                     g_setUp = false;
std::wstring             g_folder;
std::vector<std::string> g_say;

struct Command {
	enum Kind { Edit, Restore, PropOff, PropOn, StatEdit, StatRestore, StatLine, StatRemoveAsk, StatGameView, StatAdv, AffixEdit, AffixRestore, AffixOff, AffixOn, AffixTest } kind = Edit; // Edit: slot = property row, percent = cell, a = text
	int                                slot = 0, percent = 0;
	std::string                        a, b;
	D2RL::PlayerHandle                 player = D2RL::InvalidPlayerHandle;
};
std::vector<Command> g_commands;

uint16_t           g_unknownId = 0xffff; // what the game answers for a string key it does not have
std::string        g_stringsJson;        // the plugin's strings (text.json): the affix names of its own

const D2RL::ThreadService*    g_threads   = nullptr;
const D2RL::ItemService*      g_items     = nullptr;
const D2RL::InventoryService* g_inventory = nullptr;
std::atomic<bool>             g_wanted { false };
std::atomic<uint64_t>         g_askedMs { 0 };

// A stat row's cells as text (the stat card): 16 of the row, then the plugin's own name for a
// stat the game has no name for, then (unseen) the game's name when the row was changed.
constexpr int kStatCells = 17, kNameCell = 16, kWasCell = 17;
using StatCells = std::array<std::string, kStatCells + 1>;
std::map<uint16_t, StatCells> g_statEdits; // the changed and the made stats, by number
std::map<uint16_t, std::string> g_typedKeys; // string number -> the key typed for it (string-keys.txt, for Live Export)

// What Live Tables changed since the last entry (its change listener only records it, lock-free):
// put into the banks first thing under the lock, so nothing reads a table through an old pointer.
void ApplyChanges();
struct Guard {
	Guard() {
		AcquireSRWLockExclusive(&g_lock);
		ApplyChanges();
	}
	~Guard() { ReleaseSRWLockExclusive(&g_lock); }
};

void Say(const char* format, ...) {
	char    text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	LogF("%s", text);
	g_say.push_back(std::string("Live Properties: ") + text);
}

bool StatNumber(const Bank& bank, const std::string& text, uint16_t& number) {
	for (const auto& [made, cells] : g_statEdits) { // a name the plugin gave
		if (!cells[kNameCell].empty() && cells[kNameCell] == text && made < bank.stats->count) {
			number = made;
			return true;
		}
	}
	char*               end   = nullptr;
	const unsigned long typed = strtoul(text.c_str(), &end, 10);
	if (end != text.c_str() && *end == '\0') {
		number = static_cast<uint16_t>(typed);
		return typed < bank.stats->count;
	}
	// A name: the game's own stat name table. It is a hash table (bucket count, a power of two,
	// at +0x18; buckets at +0x20); anything else is not handed to the lookup. An unknown name is
	// refused - the game's "param" function would quietly make it stat 0 (strength).
	void* names = nullptr;
	memcpy(&names, static_cast<const uint8_t*>(bank.tables) + kStatNamesAt, sizeof(names));
	uint64_t    buckets = 0;
	const void* first   = nullptr;
	if (!Readable(names, 0x28)) {
		return false;
	}
	memcpy(&buckets, static_cast<const uint8_t*>(names) + 0x18, sizeof(buckets));
	memcpy(&first, static_cast<const uint8_t*>(names) + 0x20, sizeof(first));
	if (buckets == 0 || (buckets & (buckets - 1)) != 0 || buckets > (1u << 24) || !Readable(first, sizeof(void*))) {
		return false;
	}
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%s", text.c_str());
	const int found = g_link.lookup(names, buffer, 0);
	number          = static_cast<uint16_t>(found);
	return found >= 0 && static_cast<uint64_t>(found) < bank.stats->count;
}

// Does this table link its op stats the way prop_core.h says? Every op row the game read is
// looked up in the two lists it should be in. One miss and the plugin leaves the table alone.
bool LayoutHolds(const Bank& bank, int& seen) {
	const uint8_t* rows  = bank.stats->rows;
	const size_t   count = bank.stats->count;
	seen                 = 0;
	for (size_t i = 0; i < count; ++i) {
		const uint8_t* row = rows + i * prop::kStatRow;
		const uint8_t  op  = row[prop::kOpAt];
		uint16_t       base, target;
		memcpy(&base, row + prop::kOpBase, 2);
		memcpy(&target, row + prop::kOpStat, 2);
		if (op == 0 || op > 13 || base >= count || target >= count) {
			continue;
		}
		const uint8_t* a     = rows + base * prop::kStatRow;
		const uint8_t* t     = rows + target * prop::kStatRow;
		bool           inA   = false, inT = false;
		int            aUsed = 0, tUsed = 0;
		for (; aUsed < prop::kDependentMax; ++aUsed) {
			uint16_t entry;
			memcpy(&entry, a + prop::kDependents + aUsed * 2, 2);
			if (entry >= count) {
				break;
			}
			inA = inA || entry == i;
		}
		for (; tUsed < prop::kOpListMax && t[prop::kOpList + tUsed * 6 + 4] != 0; ++tUsed) {
			uint16_t entryBase, entryStat;
			memcpy(&entryBase, t + prop::kOpList + tUsed * 6, 2);
			memcpy(&entryStat, t + prop::kOpList + tUsed * 6 + 2, 2);
			inT = inT || (entryStat == i && entryBase == base && t[prop::kOpList + tUsed * 6 + 4] == op && t[prop::kOpList + tUsed * 6 + 5] == row[prop::kOpParam]);
		}
		if ((!inA && aUsed < prop::kDependentMax) || (!inT && tUsed < prop::kOpListMax) || a[prop::kHasOp] != 1 || t[prop::kIsTarget] != 1) {
			LogF("data version %d: stat %zu (op %d, of stat %d, to stat %d) is not linked the way the plugin expects", bank.version, i, op, base, target);
			return false;
		}
		++seen;
	}
	return seen > 0;
}


// ---------------------------------------------------------------- the item's line of text

// Names for affixes that are not one of the game's string keys: the keys Name1..Name16 of the
// plugin's own text.json. NOTHING is ever written into D2RCore's string memory (its tables are
// rebuilt when strings are added, another plugin registers some, or the language changes - a write
// through an old pointer would corrupt the heap). So an own-words name is kept (affix-edits.txt) and
// shown on the card, while the game shows the slot's fixed text until Live Strings gives it a key.
constexpr int kNameSlots = 16, kNameTextRoom = 40;
uint16_t      g_nameIds[kNameSlots] = {}; // the slots' string numbers (0 = not there)
std::string   g_nameText[kNameSlots];     // the own words each slot stands for ("" = free)
std::map<std::string, int> g_nameSlotOf;     // affix ("prefix 9") -> its name slot
std::string NamePlaceholder(int slot) {
	std::string text = "live affix name " + std::to_string(slot) + " ";
	text.resize(kNameTextRoom, '.');
	return text;
}

// The strings file handed to the game at plugin start: keys Name1..Name16 in the plugin's own
// namespace, the same text for every language (a missing language shows nothing).
void RegisterStrings(const D2RL::ResourceService* resources) {
	static const char* const kLanguages[] = { "enUS", "zhTW", "deDE", "esES", "frFR", "itIT", "koKR", "plPL", "esMX", "jaJP", "ptBR", "ruRU", "zhCN" };
	g_stringsJson = "[\n";
	for (int slot = 1; slot <= kNameSlots; ++slot) {
		g_stringsJson += "  { \"Key\": \"Name" + std::to_string(slot) + "\"";
		for (const char* language : kLanguages) {
			g_stringsJson += std::string(", \"") + language + "\": \"" + NamePlaceholder(slot) + "\"";
		}
		g_stringsJson += slot < kNameSlots ? " },\n" : " }\n";
	}
	g_stringsJson += "]\n";
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.path       = "data/local/lng/strings/d2rloader/live-properties/text.json",
		.bytes      = g_stringsJson.data(),
		.byteCount  = g_stringsJson.size(),
	};
	D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
	const auto                          result = resources->registerResource(g_ctx, &resource, &handle);
	if (result != D2RL::Resources::Result::Success) {
		LogF("text: the strings could not be given to the game (result %u); items will show no line", static_cast<unsigned>(result));
	}
}

// The number the game gives an unknown key, and the slots' numbers. Only numbers are asked for;
// no pointer into the game's strings is kept or written through.
void FindStrings() {
	if (g_link.stringId == nullptr || g_link.stringById == nullptr) {
		LogF("text: this D2RLoader's D2RCore.dll does not offer the string functions; items will show no line");
		return;
	}
	struct View {
		const char* text;
		size_t      length;
	};
	const char     nonsense[] = "live-properties:NoSuchKey";
	const View     none { nonsense, sizeof(nonsense) - 1 };
	const uint16_t unknown = g_link.stringId(&none);
	g_unknownId            = unknown;
	int names = 0;
	for (int slot = 1; slot <= kNameSlots; ++slot) {
		const std::string key = "live-properties:Name" + std::to_string(slot);
		const View        view { key.data(), key.size() };
		const uint16_t    id  = g_link.stringId(&view);
		g_nameIds[slot - 1]   = id != unknown ? id : 0;
		names += id != unknown ? 1 : 0;
	}
	if (names != kNameSlots) {
		LogF("text: %d of %d affix name strings found; affixes can use the game's own names only beyond that", names, kNameSlots);
	}
}

// ---------------------------------------------------------------- making the game recalculate
//
// The game keeps each unit's worked-out totals and only works a stat out again when a stat it
// is made from changes on that unit. So after the table rows changed, the property's stat is
// touched (+1, then -1) on every item the character wears or carries that has it.

struct Touch {
	uint16_t stat;
	int      done;
};

void __cdecl TouchItem(const D2RL::PluginContext*, void* item, void* userData) noexcept {
	auto* touch = static_cast<Touch*>(userData);
	uint32_t type = 0;
	if (!Readable(item, 0x1c0)) {
		return;
	}
	memcpy(&type, item, sizeof(type));
	if (type != 4) { // not an item unit
		return;
	}
	g_link.addStat(item, touch->stat, 1, 0);
	g_link.addStat(item, touch->stat, -1, 0);
	++touch->done;
}

D2RL::Inventory::IterationAction __cdecl CollectItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept {
	static_cast<std::vector<D2RL::ItemHandle>*>(userData)->push_back(item->handle);
	return D2RL::Inventory::IterationAction::Continue;
}

// Returns the number of items touched, or -1 when it cannot be done here.
int Recalculate(D2RL::PlayerHandle player, uint16_t stat, bool touchThem = true) { // false: only counts the items that have it
	if ((touchThem && g_link.addStat == nullptr) || g_items == nullptr || g_inventory == nullptr || player == D2RL::InvalidPlayerHandle
		|| !D2RL::HasItemServiceField(g_items, D2RL::ItemServiceDetailsFieldEnd)
		|| !D2RL::HasInventoryServiceField(g_inventory, offsetof(D2RL::InventoryService, forEachInventoryItem) + sizeof(void*))) {
		return -1;
	}
	std::vector<D2RL::ItemHandle>     items;
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment) | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)
		               | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cursor),
	};
	if (g_inventory->forEachInventoryItem(g_ctx, player, &filter, CollectItem, &items) != D2RL::Inventory::Result::Success) {
		return -1;
	}
	Touch touch { stat, 0 };
	for (const D2RL::ItemHandle item : items) {
		D2RL::Items::ItemDetails details { .structSize = D2RL::Items::ItemDetailsSize };
		D2RL::Items::ItemStatValue stats[160];
		if (g_items->getItemDetails(g_ctx, item, &details, stats, 160) != D2RL::Items::Result::Success) {
			continue;
		}
		for (uint32_t i = 0; i < details.statCount && i < 160; ++i) {
			if (stats[i].statId == stat) {
				if (touchThem) {
					static_cast<void>(g_items->editNativeItem(g_ctx, item, TouchItem, &touch));
				} else {
					++touch.done;
				}
				break;
			}
		}
	}
	return touch.done;
}

// The game's property table, edited in place (its rows hold their own number first). The rows as
// the game read them are kept for "The game's own row".
bool TakeProperties(Bank& bank) {
	Vector* v = bank.props;
	if (!Readable(v->rows, v->count * prop::kPropRow) || v->count == 0 || v->count > 0x7000) {
		bank.off = "its property table cannot be read";
		return false;
	}
	for (uint16_t n = 0; n < 4 && n < v->count; ++n) {
		uint16_t first;
		memcpy(&first, v->rows + n * prop::kPropRow, 2);
		if (first != n) {
			bank.off = "its property rows do not start with their number";
			return false;
		}
	}
	if (bank.propRows != v->rows || bank.propOrig.size() != v->count * prop::kPropRow) {
		bank.propOrig.assign(v->rows, v->rows + v->count * prop::kPropRow);
	}
	bank.propRows = v->rows;
	bank.propBase = v->count;
	return true;
}

Bank* MainBank() { // the newest data version that is on
	for (int i = 2; i >= 0; --i) {
		if (g_banks[i].on) {
			return &g_banks[i];
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------- every stat and property, for the Mod Book tabs

// The names in one of the game's name tables (the lookup at kLookupRva reads the same places:
// bucket count at +0x18, buckets at +0x20; an entry is {next, text, length, ..., number at +0x30}).
void WalkNames(const uint8_t* tables, uintptr_t at, size_t count, std::vector<std::string>& names) {
	names.assign(count, std::string());
	const uint8_t*        table   = nullptr;
	uint64_t              buckets = 0;
	const uint8_t* const* first   = nullptr;
	memcpy(&table, tables + at, sizeof(table));
	if (!Readable(table, 0x28)) {
		return;
	}
	memcpy(&buckets, table + 0x18, sizeof(buckets));
	memcpy(&first, table + 0x20, sizeof(first));
	if (buckets == 0 || (buckets & (buckets - 1)) != 0 || buckets > (1u << 20) || !Readable(first, buckets * sizeof(void*))) {
		return;
	}
	size_t steps = 0;
	for (uint64_t b = 0; b < buckets; ++b) {
		for (const uint8_t* node = first[b]; node != nullptr && steps < 100000 && Readable(node, 0x38); ++steps) {
			const char* key    = nullptr;
			uint64_t    length = 0;
			int32_t     value  = -1;
			memcpy(&key, node + 8, sizeof(key));
			memcpy(&length, node + 0x10, sizeof(length));
			memcpy(&value, node + 0x30, sizeof(value));
			if (value >= 0 && static_cast<size_t>(value) < count && length > 0 && length < 64 && names[static_cast<size_t>(value)].empty() && Readable(key, length)) {
				names[static_cast<size_t>(value)].assign(key, length);
			}
			memcpy(&node, node, sizeof(node));
		}
	}
}

// A property row's 28 cells as text: for each of its 7 lines func, stat, set, val ("" = nothing).
constexpr int kCells = 28;
constexpr int kCodeCell = kCells; // the property card's code (a rename through Live Tables, made rows only)
using Cells          = std::array<std::string, kCells>;
constexpr size_t kPropSet = 0x02, kPropVal = 0x0a, kPropFunc = 0x18, kPropStat = 0x20;

std::map<std::string, Cells> g_edits; // the changed rows, by property code ("#12" for a row without one)
std::set<std::string>        g_off;   // the disabled properties, by code: func 0 on all 7 lines in memory (property-off.txt)

// The game's own list of property funcs: how many, and does this one have a handler?
int FuncCount() {
	int32_t count = 0;
	if (g_imageBase != 0 && Readable(reinterpret_cast<void*>(g_imageBase + kFuncCountRva), 4)) {
		memcpy(&count, reinterpret_cast<void*>(g_imageBase + kFuncCountRva), 4);
	}
	return count > 0 && count <= 255 ? count : 0;
}
bool FuncWorks(int func) {
	void* handler = nullptr;
	if (func > 0 && func < FuncCount() && Readable(reinterpret_cast<void*>(g_imageBase + kFuncTableRva + static_cast<uintptr_t>(func) * 8), 8)) {
		memcpy(&handler, reinterpret_cast<void*>(g_imageBase + kFuncTableRva + static_cast<uintptr_t>(func) * 8), 8);
	}
	return handler != nullptr;
}

// A row the game names as a filler: no name, or the older Exalted Items' "xlt_unused_<n>" padding
// (a table that still has them gets no rows of ours; Stats > New takes such a row then).
bool FillerName(const std::string& name) {
	return name.empty() || name.rfind("xlt_unused_", 0) == 0;
}

// A stat made here that the table has as a named row: the game's name for the row is the plugin's
// name for the made stat (the row this plugin added at the start, under that name).
// Made stats Live Tables keeps (made-stats.txt, one name a line): since Live Tables 0.3.2 their cells
// live in Live Tables' grown-rows.txt, not in stat-edits.txt.
std::set<std::string> g_madeStats;
std::wstring          MadeStatsPath();

bool WrittenMade(const Bank& bank, uint16_t stat) {
	if (stat < bank.statNames.size() && !bank.statNames[stat].empty() && g_madeStats.count(bank.statNames[stat]) != 0) {
		return true;
	}
	const auto made = g_statEdits.find(stat);
	return made != g_statEdits.end() && !made->second[kNameCell].empty() && stat < bank.statNames.size() && bank.statNames[stat] == made->second[kNameCell];
}

// The game's own stat (not a filler, not one made here).
bool GameNamed(const Bank& bank, uint16_t stat) {
	return stat < bank.statNames.size() && !FillerName(bank.statNames[stat]) && !WrittenMade(bank, stat);
}

std::string StatWord(const Bank& bank, uint16_t stat) {
	if (stat == prop::kNoStat) {
		return {};
	}
	if (const auto made = g_statEdits.find(stat); made != g_statEdits.end() && !made->second[kNameCell].empty() && (stat >= bank.statNames.size() || FillerName(bank.statNames[stat]))) {
		return made->second[kNameCell];
	}
	return stat < bank.statNames.size() && !bank.statNames[stat].empty() ? bank.statNames[stat] : std::to_string(stat);
}

std::string PropKey(const Bank& bank, size_t row) {
	return row < bank.propNames.size() && !bank.propNames[row].empty() ? bank.propNames[row] : "#" + std::to_string(row);
}

bool FindRow(const Bank& bank, const std::string& key, size_t& row) {
	for (row = 0; row < bank.propBase; ++row) {
		if (PropKey(bank, row) == key) {
			return true;
		}
	}
	return false;
}

Cells RowCells(const Bank& bank, const uint8_t* row) {
	Cells cells;
	for (int i = 0; i < 7; ++i) {
		uint16_t stat;
		int16_t  val;
		memcpy(&stat, row + kPropStat + i * 2, 2);
		memcpy(&val, row + kPropVal + i * 2, 2);
		cells[static_cast<size_t>(i * 4)]     = row[kPropFunc + i] != 0 ? std::to_string(row[kPropFunc + i]) : "";
		cells[static_cast<size_t>(i * 4 + 1)] = StatWord(bank, stat);
		cells[static_cast<size_t>(i * 4 + 2)] = row[kPropSet + i] != 0 ? std::to_string(row[kPropSet + i]) : "";
		cells[static_cast<size_t>(i * 4 + 3)] = val != 0 ? std::to_string(val) : "";
	}
	return cells;
}

bool Whole(const std::string& text, long least, long most, long& number) {
	number = 0;
	if (text.empty() || text == "-") {
		return least <= 0;
	}
	char* end = nullptr;
	number    = strtol(text.c_str(), &end, 10);
	return end != text.c_str() && *end == '\0' && number >= least && number <= most;
}

// Writes the cells into the row (the plugin's copy, which the game reads). Everything is
// checked first; on false nothing was written.
bool PutCells(Bank& bank, size_t row, const Cells& cells, std::string& problem) {
	if (bank.propRows == nullptr || bank.props->rows != bank.propRows || row >= bank.propBase) {
		problem = "that row is not one of the game's own property rows";
		return false;
	}
	uint8_t* at = bank.propRows + row * prop::kPropRow;
	uint8_t  next[prop::kPropRow];
	memcpy(next, at, sizeof(next));
	for (int i = 0; i < 7; ++i) {
		const std::string line = "line " + std::to_string(i + 1) + ": ";
		long              func = 0, set = 0, val = 0;
		uint16_t          stat = prop::kNoStat;
		const std::string& word = cells[static_cast<size_t>(i * 4 + 1)];
		if (!Whole(cells[static_cast<size_t>(i * 4)], 0, 255, func) || (func != 0 && !FuncWorks(static_cast<int>(func)))) {
			problem = line + "the game has no func " + cells[static_cast<size_t>(i * 4)] + " (see the Funcs tab; empty = the line does nothing)";
			return false;
		}
		if (!word.empty() && word != "-" && !StatNumber(bank, word, stat)) {
			problem = line + "there is no stat named '" + word + "' (see the Stats tab; a number works too)";
			return false;
		}
		if (!Whole(cells[static_cast<size_t>(i * 4 + 2)], 0, 255, set)) {
			problem = line + "set is a number from 0 to 255";
			return false;
		}
		if (!Whole(cells[static_cast<size_t>(i * 4 + 3)], -32768, 32767, val)) {
			problem = line + "val is a number from -32768 to 32767";
			return false;
		}
		const int16_t value = static_cast<int16_t>(val);
		next[kPropFunc + i] = static_cast<uint8_t>(func);
		next[kPropSet + i]  = static_cast<uint8_t>(set);
		memcpy(next + kPropStat + i * 2, &stat, 2);
		memcpy(next + kPropVal + i * 2, &value, 2);
	}
	memcpy(at + 2, next + 2, prop::kPropRow - 2); // the row's own number stays
	return true;
}

std::wstring EditsPath() {
	return g_folder + L"\\property-edits.txt";
}

void ReadEdits() {
	g_edits.clear();
	std::string text;
	if (!ReadWhole(EditsPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		if (line.empty() || (line[0] == '#' && (line.size() < 2 || line[1] == ' '))) {
			continue;
		}
		std::vector<std::string> parts;
		for (size_t from = 0; from <= line.size();) {
			size_t tab = line.find('\t', from);
			tab        = tab == std::string::npos ? line.size() : tab;
			parts.push_back(line.substr(from, tab - from));
			from = tab + 1;
		}
		if (parts.size() != kCells + 1 || parts[0].empty()) {
			LogF("property-edits.txt: a line with %zu cells (29 wanted) was skipped: %s", parts.size(), line.c_str());
			continue;
		}
		Cells cells;
		for (int i = 0; i < kCells; ++i) {
			cells[static_cast<size_t>(i)] = parts[static_cast<size_t>(i + 1)] == "-" ? "" : parts[static_cast<size_t>(i + 1)];
		}
		g_edits[parts[0]] = cells;
	}
}

void WriteEdits() {
	std::string text = "# Live Properties: the game's property rows that were changed in the Properties tab.\r\n"
	                   "# property code, then for each of its 7 lines: func, stat, set, val (- = nothing). Tabs between.\r\n"
	                   "# Written by the plugin. Read when the game has read its tables. Delete a line to get the game's own row back.\r\n";
	for (const auto& [key, cells] : g_edits) {
		text += key;
		for (const std::string& cell : cells) {
			text += "\t" + (cell.empty() ? std::string("-") : cell);
		}
		text += "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(EditsPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(EditsPath()).c_str());
	}
}

void ApplyEdits(Bank& bank) {
	int done = 0;
	for (const auto& [key, cells] : g_edits) {
		size_t      row = 0;
		std::string problem;
		if (!FindRow(bank, key, row)) {
			if (bank.version == 3) {
				Say("property-edits.txt: the game has no property '%s'; that line does nothing", key.c_str());
			}
		} else if (!PutCells(bank, row, cells, problem)) {
			Say("property-edits.txt, data version %d, property '%s': %s", bank.version, key.c_str(), problem.c_str());
		} else {
			++done;
		}
	}
	if (!g_edits.empty()) {
		LogF("data version %d: %d of %zu changed property rows put back", bank.version, done, g_edits.size());
	}
}

// ---- made properties (made-properties.txt): codes of the user's own. Each gets a row as the game
// reads properties.txt (table rows, every data version), so the mod's own files, the Affixes tab and
// Live Cube Recipes can name it. Its cells are in property-edits.txt under the code, like any edited
// row. Its row is grown by Live Tables (New property) and kept by Live Tables.
struct Made {
	std::string code, was; // was: the game's name for its row when the line was written ("" = none)
};
std::vector<Made> g_made; // in the order of the file = the order its rows are added

std::wstring MadePath() {
	return g_folder + L"\\made-properties.txt";
}

void ReadMade() {
	g_made.clear();
	std::string text;
	if (!ReadWhole(MadePath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		const size_t tab = line.find('\t');
		if (line.empty() || line[0] == '#' || tab == 0) {
			continue;
		}
		g_made.push_back({ line.substr(0, tab), tab == std::string::npos ? std::string() : line.substr(tab + 1) });
	}
}

void WriteMade() {
	std::string text = "# Live Properties: the properties made on the Properties tab, in the order their rows are added.\r\n"
	                   "# code, then the game's name for its row when this was written (empty = none). Tabs between.\r\n"
	                   "# Their lines are in property-edits.txt under the code. Written by the plugin.\r\n";
	for (const Made& made : g_made) {
		text += made.code + "\t" + made.was + "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(MadePath(), text)) {
		Say("could not write %s - the new property holds until the game closes", Narrow(MadePath()).c_str());
	}
}

const Made* MadeByCode(const std::string& code) {
	for (const Made& made : g_made) {
		if (made.code == code) {
			return &made;
		}
	}
	return nullptr;
}

// After a start: a made property whose row the game now reads under the code itself.
void RefreshMade(const Bank& bank) {
	if (bank.version != 3) {
		return;
	}
	bool changed = false;
	for (Made& made : g_made) {
		for (const std::string& name : bank.propNames) {
			if (name == made.code && made.was != made.code) {
				made.was = made.code;
				changed  = true;
			}
		}
	}
	if (changed) {
		WriteMade();
	}
}

// ---- disabled properties (property-off.txt): the row stays, so every code that names it still
// parses; its 7 funcs are 0 in memory, so it gives nothing to items made from then on. The cells
// are not lost: they are the edited cells (property-edits.txt) or the game's own row.

std::wstring OffPath() {
	return g_folder + L"\\property-off.txt";
}

void ReadOff() {
	g_off.clear();
	std::string text;
	if (!ReadWhole(OffPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (!line.empty() && !(line[0] == '#' && (line.size() < 2 || line[1] == ' '))) {
			g_off.insert(line);
		}
	}
}

void WriteOff() {
	std::string text = "# Live Properties: the properties disabled on their card (one code a line). A disabled property does nothing:\r\n"
	                   "# its 7 funcs are 0 in the game's memory; the row stays, so recipes, affixes and items that name it still load.\r\n"
	                   "# Written by the plugin. Delete a line to enable that property again.\r\n";
	for (const std::string& key : g_off) {
		text += key + "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(OffPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(OffPath()).c_str());
	}
}

// What the row holds when it is on: the edited cells, or the game's own row.
Cells PropLogical(const Bank& bank, size_t row) {
	if (const auto edited = g_edits.find(PropKey(bank, row)); edited != g_edits.end()) {
		return edited->second;
	}
	if ((row + 1) * prop::kPropRow <= bank.propOrig.size()) {
		return RowCells(bank, bank.propOrig.data() + row * prop::kPropRow);
	}
	return RowCells(bank, bank.propRows + row * prop::kPropRow);
}

void FuncsOff(Bank& bank, size_t row) {
	memset(bank.propRows + row * prop::kPropRow + kPropFunc, 0, 7);
}

void ApplyOff(Bank& bank) {
	int done = 0;
	for (const std::string& key : g_off) {
		size_t row = 0;
		if (bank.props->rows == bank.propRows && FindRow(bank, key, row)) {
			FuncsOff(bank, row);
			++done;
		} else if (bank.version == 3) {
			Say("property-off.txt: the game has no property '%s'; that line does nothing", key.c_str());
		}
	}
	if (!g_off.empty()) {
		LogF("data version %d: %d of %zu disabled properties are off", bank.version, done, g_off.size());
	}
}

// ---------------------------------------------------------------- any stat's row (the stat card)

enum { kCellNumber, kCellStat, kCellString, kCellSigned };
enum { kGroupSave, kGroupText, kGroupOp };
struct Field {
	const char* name;
	size_t      at;
	int         size, kind;
	long        least, most;
	int         group;
	const char* tip;
};
constexpr Field kFields[kNameCell] = {
	{ "signed", prop::kFlags, 4, kCellSigned, 0, 1, kGroupSave, "1 = the number on an item may be negative. Part of how items are SAVED." },
	{ "send bits", prop::kSendBits, 1, kCellNumber, 0, 32, kGroupSave, "Bits the number takes when the game sends it. Part of how items are saved and sent." },
	{ "save bits", prop::kSaveBits, 1, kCellNumber, 0, 32, kGroupSave, "Bits the number takes on a saved item: 9 bits hold 0-511 (or -256..255 when signed). 0 = not saved on items." },
	{ "save add", prop::kSaveAdd, 4, kCellNumber, 0, 0x7fffffff, kGroupSave, "Added before saving so a negative number fits: with save add 32 the item can hold -32 and up." },
	{ "val shift", prop::kValShift, 1, kCellNumber, 0, 16, kGroupSave, "8 = the game keeps the stat 256 times larger than shown (life, mana, stamina). Changes what every number of it means." },
	{ "desc priority", prop::kDescPriority, 2, kCellNumber, 0, 65535, kGroupText, "Where the line stands on an item: higher = further up." },
	{ "desc func", prop::kDescFunc, 1, kCellNumber, 0, 40, kGroupText, "How the line is written: 1 '+N text', 2 'N% text', 3 'N text', 4 '+N% text' ... 0 = no line." },
	{ "desc val", prop::kDescVal, 1, kCellNumber, 0, 2, kGroupText, "0 = no number in the line, 1 = number first, 2 = number last." },
	{ "desc str pos", prop::kDescStrPos, 2, kCellString, 0, 65535, kGroupText, "The line's text for a positive number: a string key (ModStr1a) or the number of a string a stat already uses." },
	{ "desc str neg", prop::kDescStrNeg, 2, kCellString, 0, 65535, kGroupText, "The same for a negative number." },
	{ "op", prop::kOpAt, 1, kCellNumber, 0, 13, kGroupOp, "What the stat does to other stats, 1-13 (2 = adds value x op base >> op param to the op stats). Empty = nothing." },
	{ "op param", prop::kOpParam, 1, kCellNumber, 0, 31, kGroupOp, "The op's number: the result is divided by 2 to this power." },
	{ "op base", prop::kOpBase, 2, kCellStat, 0, 0, kGroupOp, "The stat the op multiplies by (level, maxhp ...). A name from the Stats list, or its number." },
	{ "op stat 1", prop::kOpStat, 2, kCellStat, 0, 0, kGroupOp, "The stat the result is added to." },
	{ "op stat 2", prop::kOpStat + 2, 2, kCellStat, 0, 0, kGroupOp, "A second stat it is added to (only read when op stat 1 is filled)." },
	{ "op stat 3", prop::kOpStat + 4, 2, kCellStat, 0, 0, kGroupOp, "A third one." },
};

long FieldValue(const uint8_t* row, const Field& field) {
	if (field.size == 1) {
		return row[field.at];
	}
	if (field.size == 2) {
		uint16_t v;
		memcpy(&v, row + field.at, 2);
		return v;
	}
	int32_t v;
	memcpy(&v, row + field.at, 4);
	return field.kind == kCellSigned ? ((v & static_cast<int32_t>(prop::kFlagSigned)) != 0 ? 1 : 0) : v;
}

StatCells StatRowCells(const Bank& bank, const uint8_t* row) {
	StatCells cells;
	for (int i = 0; i < kNameCell; ++i) {
		const long v                  = FieldValue(row, kFields[i]);
		cells[static_cast<size_t>(i)] = kFields[i].kind == kCellStat ? StatWord(bank, static_cast<uint16_t>(v)) : v != 0 ? std::to_string(v) : "";
	}
	return cells;
}

// Is this string number one the table already shows?
bool StringInUse(const Bank& bank, uint16_t id) {
	for (size_t i = 0; i < bank.stats->count; ++i) {
		uint16_t pos, neg;
		memcpy(&pos, bank.stats->rows + i * prop::kStatRow + prop::kDescStrPos, 2);
		memcpy(&neg, bank.stats->rows + i * prop::kStatRow + prop::kDescStrNeg, 2);
		if (pos == id || neg == id) {
			return true;
		}
	}
	return false;
}

// The row the cells describe, in `next`. Everything is checked; the table is not written.
bool BuildStatRow(const Bank& bank, uint16_t stat, const StatCells& cells, uint8_t* next, std::string& problem) {
	const uint8_t* row = bank.stats->rows + stat * prop::kStatRow;
	memcpy(next, row, prop::kStatRow);
	for (int i = 0; i < kNameCell; ++i) {
		const Field&       field = kFields[i];
		const std::string& text  = cells[static_cast<size_t>(i)];
		long               value = 0;
		if (field.kind == kCellStat) {
			uint16_t other = prop::kNoStat;
			if (!text.empty() && text != "-" && !StatNumber(bank, text, other)) {
				problem = std::string(field.name) + ": there is no stat named '" + text + "'";
				return false;
			}
			value = other;
		} else if (field.kind == kCellString && !text.empty() && text != "-" && (text[0] < '0' || text[0] > '9')) {
			struct View {
				const char* text;
				size_t      length;
			};
			const View     view { text.data(), text.size() };
			const uint16_t id = g_link.stringId != nullptr ? g_link.stringId(&view) : g_unknownId;
			if (g_link.stringId == nullptr || id == g_unknownId) {
				problem = std::string(field.name) + ": the game has no string with the key '" + text + "'";
				return false;
			}
			value          = id;
			g_typedKeys[id] = text; // the stat card keeps numbers; an export needs the key
		} else if (!Whole(text, field.least, field.most, value)) {
			problem = std::string(field.name) + " is a number from " + std::to_string(field.least) + " to " + std::to_string(field.most);
			return false;
		} else if (field.kind == kCellString && value != 0 && value != FieldValue(row, field) && !StringInUse(bank, static_cast<uint16_t>(value))) {
			problem = std::string(field.name) + ": no stat uses string number " + text + ", so the plugin cannot tell that it exists. Type the string's key instead.";
			return false;
		}
		if (field.kind == kCellSigned) {
			uint32_t flags;
			memcpy(&flags, next + field.at, 4);
			flags = value != 0 ? flags | prop::kFlagSigned : flags & ~prop::kFlagSigned;
			memcpy(next + field.at, &flags, 4);
		} else if (field.size == 1) {
			next[field.at] = static_cast<uint8_t>(value);
		} else if (field.size == 2) {
			const uint16_t v = static_cast<uint16_t>(value);
			memcpy(next + field.at, &v, 2);
		} else {
			const int32_t v = static_cast<int32_t>(value);
			memcpy(next + field.at, &v, 4);
		}
	}
	const std::string& name = cells[kNameCell];
	if (!name.empty()) {
		uint16_t other = 0;
		bool     plain = name.size() <= 40 && !(name[0] >= '0' && name[0] <= '9');
		for (const char letter : name) {
			plain = plain && (isalnum(static_cast<unsigned char>(letter)) != 0 || letter == '_' || letter == '-');
		}
		if (!plain) {
			problem = "name: letters, digits, _ and - only, not starting with a digit, 40 at most";
			return false;
		}
		if (StatNumber(bank, name, other) && other != stat) {
			problem = "name: stat " + std::to_string(other) + " already has the name '" + name + "'";
			return false;
		}
	}
	return true;
}

// What the Save Scan plugin found, for the card's note.
struct ScanText {
	std::string hits, problems;
	int         shown = 0;
};
void __cdecl OnScanHit(void* user, const savescan::Hit* hit) noexcept {
	auto* text = static_cast<ScanText*>(user);
	if (hit != nullptr && text->shown++ < 4) {
		text->hits += std::string(text->hits.empty() ? "" : "; ") + (hit->file != nullptr ? hit->file : "?") + " (" + (hit->code != nullptr ? hit->code : "?") + ", " + (hit->where != nullptr ? hit->where : "?") + ")";
	}
}
void __cdecl OnScanProblem(void* user, const char* file, const char* why) noexcept {
	auto* text = static_cast<ScanText*>(user);
	if (text->problems.size() < 300) {
		text->problems += std::string(text->problems.empty() ? "" : "; ") + (file != nullptr && file[0] != '\0' ? std::string(file) + ": " : "") + (why != nullptr ? why : "?");
	}
}

bool GroupDiffers(const uint8_t* a, const uint8_t* b, int group) {
	for (const Field& field : kFields) {
		if (field.group == group && FieldValue(a, field) != FieldValue(b, field)) {
			return true;
		}
	}
	return false;
}

// Makes the stat's row what the cells say, with the safeguards:
//  * everything is checked before anything is written;
//  * the save shape does not change while the character wears or carries an item with the stat;
//  * an op is unlinked, the game made to work its targets out again, then linked anew - the
//    game's own way (prop_core LinkOp), with its limits (16 additions, 64 dependents);
//  * a changed line of text is followed by the game's own rebuild of its line order.
// `items`: check them (false when the saved changes are put back after the tables were read).
bool ApplyStat(Bank& bank, uint16_t stat, const StatCells& cells, D2RL::PlayerHandle player, bool items, std::string& problem, int* unchecked) {
	if (stat >= bank.stats->count) {
		problem = "there is no stat " + std::to_string(stat);
		return false;
	}
	uint8_t* rows = bank.stats->rows;
	uint8_t* row  = rows + stat * prop::kStatRow;
	uint8_t  next[prop::kStatRow];
	if (!BuildStatRow(bank, stat, cells, next, problem)) {
		return false;
	}
	const bool save = GroupDiffers(row, next, kGroupSave), text = GroupDiffers(row, next, kGroupText), op = GroupDiffers(row, next, kGroupOp);
	uint16_t   base, targets[3];
	memcpy(&base, next + prop::kOpBase, 2);
	memcpy(targets, next + prop::kOpStat, 6);
	if (op && next[prop::kOpAt] != 0 && !prop::LinkOp(rows, bank.stats->count, stat, next[prop::kOpAt], next[prop::kOpParam], base, targets, true, problem)) {
		return false;
	}
	if (save && items) {
		const int carried = Recalculate(player, stat, false);
		if (carried > 0) {
			problem = std::to_string(carried) + (carried == 1 ? " item" : " items") + " you wear or carry (or hold on the cursor) " + (carried == 1 ? "has" : "have")
			        + " this stat. Its saved shape (signed, bits, save add, val shift) cannot change while they exist: the game could not read them back. Sell or drop them first.";
			return false;
		}
		// every saved character and the shared stash, as they are on disk (the Save Scan plugin; it runs only here)
		int saved = 2; // 1 = looked at and clear, 2 = Save Scan is not installed
		if (const savescan::Api* scan = savescan::Find(); scan != nullptr) {
			savescan::Summary sum { sizeof(sum), 0, 0, 0, 0, 0 };
			ScanText          found;
			static_cast<void>(scan->scan(&stat, 1, &sum, OnScanHit, OnScanProblem, &found));
			LogF("save scan for stat %d: ran %u, %u files, %u unreadable, %u items, %u hold it. %s %s", stat, sum.ran, sum.files, sum.unreadable, sum.items, sum.holders, found.hits.c_str(), found.problems.c_str());
			if (sum.ran == 0) {
				problem = "the saved characters could not be checked (" + found.problems + "), so the stat's saved shape stays as it is.";
				return false;
			}
			if (sum.holders != 0) {
				problem = std::to_string(sum.holders) + (sum.holders == 1 ? " saved item holds" : " saved items hold") + " this stat: " + found.hits + (sum.holders > 4 ? " ..." : "")
				        + ". Its saved shape cannot change while they exist. (Console: savescan " + std::to_string(stat) + " lists them all.)";
				return false;
			}
			if (sum.unreadable != 0) {
				problem = std::to_string(sum.unreadable) + " save file(s) could not be read, so what they hold is not known (" + found.problems + "). The stat's saved shape stays as it is.";
				return false;
			}
			saved = 1;
		}
		if (unchecked != nullptr) {
			*unchecked = (carried < 0 ? 1 : 0) | (saved << 1);
		}
	}
	if (op) {
		prop::UnlinkOp(rows, bank.stats->count, stat);
		static_cast<void>(Recalculate(player, stat)); // its old targets, now without it
		if (next[prop::kOpAt] != 0) {
			static_cast<void>(prop::LinkOp(rows, bank.stats->count, stat, next[prop::kOpAt], next[prop::kOpParam], base, targets, false, problem));
		} else {
			prop::ClearOp(rows, bank.stats->count, stat);
			row[prop::kOpParam] = next[prop::kOpParam]; // no op: these cells are kept as typed and do nothing
			memcpy(row + prop::kOpBase, next + prop::kOpBase, 2);
			memcpy(row + prop::kOpStat, next + prop::kOpStat, 6);
		}
	}
	for (const Field& field : kFields) {
		if (field.group == kGroupOp) {
			continue;
		}
		if (field.kind == kCellSigned) {
			uint32_t flags, wanted;
			memcpy(&flags, row + field.at, 4);
			memcpy(&wanted, next + field.at, 4);
			flags = (flags & ~prop::kFlagSigned) | (wanted & prop::kFlagSigned);
			memcpy(row + field.at, &flags, 4);
		} else {
			memcpy(row + field.at, next + field.at, static_cast<size_t>(field.size));
		}
	}
	if (op) {
		static_cast<void>(Recalculate(player, stat));
	}
	if (text && g_link.descOrder != nullptr) {
		g_link.descOrder(bank.tables);
	}
	return true;
}

std::wstring StatEditsPath() {
	return g_folder + L"\\stat-edits.txt";
}

std::wstring MadeStatsPath() {
	return g_folder + L"\\made-stats.txt";
}
void ReadMadeStats() {
	g_madeStats.clear();
	std::string text;
	if (!ReadWhole(MadeStatsPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (!line.empty() && line[0] != '#') {
			g_madeStats.insert(line);
		}
	}
}
void WriteMadeStats() {
	std::string text = "# Live Properties: the stats made here. Live Tables keeps their rows and cells (grown-rows.txt). One name a line. Written by the plugin.\r\n";
	for (const std::string& name : g_madeStats) {
		text += name + "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(MadeStatsPath(), text)) {
		Say("could not write %s", Narrow(MadeStatsPath()).c_str());
	}
}

void ReadStatEdits() {
	g_statEdits.clear();
	std::string text;
	if (!ReadWhole(StatEditsPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> parts;
		for (size_t from = 0; from <= line.size();) {
			size_t tab = line.find('\t', from);
			tab        = tab == std::string::npos ? line.size() : tab;
			parts.push_back(line.substr(from, tab - from));
			from = tab + 1;
		}
		const long number = parts.size() == kStatCells + 2 ? atol(parts[0].c_str()) : -1;
		if (number < 0 || number > 0x7fff || parts[0].empty() || parts[0][0] < '0' || parts[0][0] > '9') {
			LogF("stat-edits.txt: a line was skipped (%zu cells, %d wanted): %s", parts.size(), kStatCells + 2, line.c_str());
			continue;
		}
		StatCells cells;
		for (int i = 0; i <= kStatCells; ++i) {
			cells[static_cast<size_t>(i)] = parts[static_cast<size_t>(i + 1)] == "-" ? "" : parts[static_cast<size_t>(i + 1)];
		}
		g_statEdits[static_cast<uint16_t>(number)] = cells;
	}
}

void WriteStatEdits() {
	std::string text = "# Live Properties: the stat rows that were changed or made in the Stats tab.\r\n# stat number, then";
	for (const Field& field : kFields) {
		text += std::string(" ") + field.name + ",";
	}
	text += " the plugin's name for a made stat, the game's name for the row (- = nothing). Tabs between.\r\n"
	        "# Written by the plugin. Read when the game has read its tables. Delete a line to get the game's own row back.\r\n";
	for (const auto& [stat, cells] : g_statEdits) {
		text += std::to_string(stat);
		for (const std::string& cell : cells) {
			text += "\t" + (cell.empty() ? std::string("-") : cell);
		}
		text += "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(StatEditsPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(StatEditsPath()).c_str());
	}
	std::string keys = "# Live Properties: string keys typed into stat cells, by the number the game gave them (for Live Export).\r\n";
	for (const auto& [id, key] : g_typedKeys) {
		keys += std::to_string(id) + "\t" + key + "\r\n";
	}
	static_cast<void>(WriteOwn(g_folder + L"\\string-keys.txt", keys));
}

std::string GameStatName(const Bank& bank, uint16_t stat) {
	return stat < bank.statNames.size() ? bank.statNames[stat] : std::string();
}

// A saved line's string numbers that no stat of this start uses (a made stat's line taken from an old
// filler row, e.g. 5382) are left empty this start, so the rest of the line still counts; the file
// is not changed. A number that was typed as a key (string-keys.txt) is looked up by its key again.
StatCells Strings(const Bank& bank, uint16_t stat, StatCells cells) {
	const uint8_t* row = stat < bank.stats->count ? bank.stats->rows + stat * prop::kStatRow : nullptr;
	for (int i = 0; i < kNameCell && row != nullptr; ++i) {
		const Field&       field = kFields[i];
		const std::string& text  = cells[static_cast<size_t>(i)];
		long               value = 0;
		if (field.kind != kCellString || text.empty() || text[0] < '0' || text[0] > '9' || !Whole(text, field.least, field.most, value) || value == 0 || value == FieldValue(row, field)
			|| StringInUse(bank, static_cast<uint16_t>(value))) {
			continue;
		}
		if (const auto key = g_typedKeys.find(static_cast<uint16_t>(value)); key != g_typedKeys.end()) {
			cells[static_cast<size_t>(i)] = key->second; // typed as a key: found by its key again (its number may differ this start)
		} else {
			if (bank.version == 3) {
				LogF("stat-edits.txt, stat %d: %s %s is no string any stat uses this start - left empty (the file keeps it)", stat, field.name, text.c_str());
			}
			cells[static_cast<size_t>(i)].clear();
		}
	}
	return cells;
}

bool StatDisabledIn(const Bank& bank, uint16_t stat);

void ApplyStatEdits(Bank& bank) {
	int done = 0;
	for (const auto& [stat, cells] : g_statEdits) {
		std::string problem;
		if (!cells[kNameCell].empty() && (stat >= bank.stats->count || GameStatName(bank, stat) != cells[kNameCell]) && GameStatName(bank, stat) != cells[kWasCell]) {
			continue; // a made stat whose row is not at its number: the handover to Live Tables sees to it (log: "handover")
		}
		if (StatDisabledIn(bank, stat)) {
			continue; // Live Tables keeps it disabled; Enable puts the line back (StatLine)
		}
		if (stat >= bank.stats->count || (GameStatName(bank, stat) != cells[kWasCell] && !WrittenMade(bank, stat))) {
			if (bank.version == 3) {
				Say("stat-edits.txt: stat %d is not '%s' in the game's table any more; that line does nothing", stat, cells[kWasCell].empty() ? "(an empty row)" : cells[kWasCell].c_str());
			}
		} else if (!ApplyStat(bank, stat, Strings(bank, stat, cells), D2RL::InvalidPlayerHandle, false, problem, nullptr)) {
			Say("stat-edits.txt, data version %d, stat %d: %s", bank.version, stat, problem.c_str());
		} else {
			++done;
		}
	}
	if (!g_statEdits.empty()) {
		LogF("data version %d: %d of %zu changed stat rows put back", bank.version, done, g_statEdits.size());
	}
}

void BuildViews();
void AffixIdTest();
void RunEdit(const Command& command);
void RunStat(const Command& command);
void RunAffix(const Command& command);

// Looks at the tables as they are now and puts every saved property back. Game thread, or the
// tables-read notice.
// ---------------------------------------------------------------- the magic affixes (the Affixes tab)
//
// Every prefix and suffix row can be changed, and an empty row made into a new affix. A recipe
// forces one with pre=N / suf=N (N = its number among the prefixes or suffixes, the first row 0);
// the game takes a forced affix only if it may roll at all: spawnable 1, frequency above 0, its
// level reached, and the item's type among its itypes. A charm always gets at least one affix.
// Saved items keep their stats and only name their affixes by number, so a changed row changes
// the name existing items show and nothing else.

enum AffixKind { kSuffixes, kPrefixes, kAutomagic };
const char* const kAffixKinds[] = { "suffix", "prefix", "automagic" };

struct AffixEdit {
	int          kind  = kPrefixes;
	size_t       index = 0;
	std::string  was;  // the row's name as the game read it ("" = an empty row): a line only counts on that row
	affix::Cells cells;
};
std::map<std::string, AffixEdit> g_affixEdits; // by "prefix 9"

std::string AffixKey(int kind, size_t index) {
	return std::string(kAffixKinds[kind]) + " " + std::to_string(index);
}

// A row of the vector: its kind and its number among that kind; and back.
void AffixPlace(const Bank& bank, size_t row, int& kind, size_t& index) {
	kind  = row < bank.preBase ? kSuffixes : row < bank.autoBase ? kPrefixes : kAutomagic;
	index = row - (kind == kSuffixes ? 0 : kind == kPrefixes ? bank.preBase : bank.autoBase);
}
bool AffixRowOf(const Bank& bank, int kind, size_t index, size_t& row) {
	const size_t first = kind == kSuffixes ? 0 : kind == kPrefixes ? bank.preBase : bank.autoBase;
	const size_t end   = kind == kSuffixes ? bank.preBase : kind == kPrefixes ? bank.autoBase : bank.affixes->count;
	row                = first + index;
	return bank.affixOn && row < end;
}

std::string RowName(const uint8_t* row) {
	return std::string(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), affix::kNameRoom - 1));
}
const uint8_t* AffixOrig(const Bank& bank, size_t row) {
	return (row + 1) * affix::kRow <= bank.affixOrig.size() ? bank.affixOrig.data() + row * affix::kRow : nullptr;
}

// A row's cells; a name the plugin gave shows its words.
affix::Cells AffixCells(const Bank& bank, size_t row) {
	affix::Cells cells = affix::Read(bank.affixes->rows + row * affix::kRow, bank.names);
	const std::string own = "live-properties:Name";
	if (cells[affix::kNameCell].rfind(own, 0) == 0) {
		const int slot = atoi(cells[affix::kNameCell].c_str() + own.size());
		cells[affix::kNameCell] = slot >= 1 && slot <= kNameSlots ? g_nameText[slot - 1] : "";
	}
	return cells;
}

void FreeName(const std::string& key) {
	if (const auto at = g_nameSlotOf.find(key); at != g_nameSlotOf.end()) {
		g_nameText[at->second].clear();
		g_nameSlotOf.erase(at);
	}
}

// The name cell into a string key and its number: the row's own name, one of the game's keys,
// or the plugin's own string with these words.
bool NameFor(const Bank& bank, size_t row, const std::string& rowKey, const std::string& text, std::string& key, uint16_t& id, std::string& problem) {
	const uint8_t* orig = AffixOrig(bank, row);
	if (orig != nullptr && text == RowName(orig)) {
		key = text;
		memcpy(&id, orig + affix::kNameId, 2);
		FreeName(rowKey);
		return true;
	}
	struct View {
		const char* text;
		size_t      length;
	};
	const View view { text.data(), text.size() };
	if (!text.empty() && text.size() < affix::kNameRoom && g_link.stringId != nullptr && g_link.stringId(&view) != g_unknownId) {
		key = text, id = g_link.stringId(&view);
		FreeName(rowKey);
		return true;
	}
	if (text.empty()) {
		key.clear(), id = g_unknownId;
		FreeName(rowKey);
		return true;
	}
	if (text.size() > static_cast<size_t>(kNameTextRoom) || text.find('"') != std::string::npos) {
		problem = "a name of your own has at most " + std::to_string(kNameTextRoom) + " letters";
		return false;
	}
	int slot = -1;
	if (const auto at = g_nameSlotOf.find(rowKey); at != g_nameSlotOf.end()) {
		slot = at->second;
	}
	for (int i = 0; i < kNameSlots && slot < 0; ++i) {
		if (g_nameIds[i] != 0 && g_nameText[i].empty()) {
			bool taken = false;
			for (const auto& [other, used] : g_nameSlotOf) {
				taken = taken || used == i;
			}
			slot = taken ? -1 : i;
		}
	}
	if (slot < 0 || g_nameIds[slot] == 0) {
		problem = "'" + text + "' is not one of the game's names, and the plugin's " + std::to_string(kNameSlots) + " names of its own are all in use";
		return false;
	}
	g_nameSlotOf[rowKey] = slot;
	g_nameText[slot]     = text; // shown on the card; the game shows the slot's fixed text (Live Strings, 1.3b, gives it the words)
	key = "live-properties:Name" + std::to_string(slot + 1);
	id  = g_nameIds[slot];
	return true;
}

// Writes the cells into the game's row. Everything is checked first; on false nothing was written.
bool PutAffix(Bank& bank, size_t row, const affix::Cells& cells, std::string& problem) {
	int    kind  = 0;
	size_t index = 0;
	AffixPlace(bank, row, kind, index);
	uint8_t* at = bank.affixes->rows + row * affix::kRow;
	uint8_t  next[affix::kRow];
	memcpy(next, at, sizeof(next));
	if (!affix::Build(cells, bank.names, next, problem)) {
		return false;
	}
	std::string key;
	uint16_t    id = 0;
	if (!NameFor(bank, row, AffixKey(kind, index), cells[affix::kNameCell], key, id, problem)) {
		return false;
	}
	memset(next, 0, affix::kNameRoom);
	memcpy(next, key.data(), key.size() < affix::kNameRoom - 1 ? key.size() : affix::kNameRoom - 1);
	memcpy(next + affix::kNameId, &id, 2);
	memcpy(at, next, sizeof(next));
	return true;
}

// Finds the tables of one data version. False: the Affixes tab shows nothing for it.
bool AffixSetUp(Bank& bank) {
	auto* tables  = static_cast<uint8_t*>(bank.tables);
	bank.affixOn  = false;
	bank.affixes  = reinterpret_cast<Vector*>(tables + kAffixesAt);
	uint8_t* pre  = nullptr;
	uint8_t* aut  = nullptr;
	memcpy(&pre, tables + kAffixPrefixAt, sizeof(pre));
	memcpy(&aut, tables + kAffixAutoAt, sizeof(aut));
	const Vector& v = *bank.affixes;
	if (v.count == 0 || v.count > 20000 || !Readable(v.rows, v.count * affix::kRow) || pre < v.rows || aut < pre || aut > v.rows + v.count * affix::kRow
		|| (pre - v.rows) % affix::kRow != 0 || (aut - v.rows) % affix::kRow != 0) {
		LogF("data version %d: the affix table is not where the plugin expects it; no Affixes tab for it", bank.version);
		return false;
	}
	const auto* types = reinterpret_cast<const Vector*>(tables + kItemTypesAt);
	if (types->count == 0 || types->count > 5000 || !Readable(types->rows, types->count * kItemTypeRow)) {
		LogF("data version %d: the item type table is not where the plugin expects it; no Affixes tab for it", bank.version);
		return false;
	}
	bank.typeCodes.assign(types->count, std::string());
	for (size_t i = 0; i < types->count; ++i) {
		std::string code(reinterpret_cast<const char*>(types->rows + i * kItemTypeRow), 4);
		while (!code.empty() && (code.back() == ' ' || code.back() == '\0')) {
			code.pop_back();
		}
		bank.typeCodes[i] = code;
	}
	bank.preBase  = static_cast<size_t>(pre - v.rows) / affix::kRow;
	bank.autoBase = static_cast<size_t>(aut - v.rows) / affix::kRow;
	if (bank.affixOrigOf != v.rows || bank.affixOrig.size() != v.count * affix::kRow) {
		bank.affixOrig.assign(v.rows, v.rows + v.count * affix::kRow);
		bank.affixOrigOf = v.rows;
	}
	bank.names           = affix::Names {};
	bank.names.props     = &bank.propNames; // the vector itself: re-walking the names (a grown table) keeps it valid
	bank.names.types     = &bank.typeCodes;
	affix::Learn(bank.affixOrig.data(), v.count, bank.names);
	bank.affixOn = true;
	LogF("data version %d: affixes - %zu suffixes, %zu prefixes, %zu automagic; %zu item types", bank.version, bank.preBase, bank.autoBase - bank.preBase,
		static_cast<size_t>(v.count) - bank.autoBase, bank.typeCodes.size());
	return true;
}

std::wstring AffixEditsPath() {
	return g_folder + L"\\affix-edits.txt";
}

void ReadAffixEdits() {
	g_affixEdits.clear();
	std::string text;
	if (!ReadWhole(AffixEditsPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> parts;
		for (size_t from = 0; from <= line.size();) {
			size_t tab = line.find('\t', from);
			tab        = tab == std::string::npos ? line.size() : tab;
			parts.push_back(line.substr(from, tab - from));
			from = tab + 1;
		}
		int kind = -1;
		for (int k = 0; k < 3 && !parts.empty(); ++k) {
			kind = parts[0] == kAffixKinds[k] ? k : kind;
		}
		if (parts.size() != affix::kCells + 3 || kind < 0 || parts[1].empty()) {
			LogF("affix-edits.txt: a line was skipped (%zu cells, %d wanted): %s", parts.size(), affix::kCells + 3, line.c_str());
			continue;
		}
		AffixEdit edit;
		edit.kind  = kind;
		edit.index = static_cast<size_t>(strtoul(parts[1].c_str(), nullptr, 10));
		edit.was   = parts[2] == "-" ? "" : parts[2];
		for (int i = 0; i < affix::kCells; ++i) {
			edit.cells[static_cast<size_t>(i)] = parts[static_cast<size_t>(i + 3)] == "-" ? "" : parts[static_cast<size_t>(i + 3)];
		}
		g_affixEdits[AffixKey(kind, edit.index)] = edit;
	}
}

void WriteAffixEdits() {
	std::string text = "# Live Properties: the magic affix rows changed or made in the Affixes tab.\r\n"
	                   "# kind (prefix / suffix / automagic), number, the row's name as the game read it, then name, spawnable, rare, level, maxlevel, levelreq, frequency, group,\r\n"
	                   "# classspecific, class, classlevelreq, mod1code, mod1param, mod1min, mod1max, mod2..., mod3..., itype1-7, etype1-5 (- = empty). Tabs between.\r\n"
	                   "# Written by the plugin. Read when the game has read its tables. Delete a line to get the game's own row back.\r\n";
	for (const auto& [key, edit] : g_affixEdits) {
		text += std::string(kAffixKinds[edit.kind]) + "\t" + std::to_string(edit.index) + "\t" + (edit.was.empty() ? "-" : edit.was);
		for (const std::string& cell : edit.cells) {
			text += "\t" + (cell.empty() ? std::string("-") : cell);
		}
		text += "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(AffixEditsPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(AffixEditsPath()).c_str());
	}
}

void ApplyAffixEdits(Bank& bank) {
	int done = 0;
	for (const auto& [key, edit] : g_affixEdits) {
		size_t         row  = 0;
		std::string    problem;
		const uint8_t* orig = AffixRowOf(bank, edit.kind, edit.index, row) ? AffixOrig(bank, row) : nullptr;
		if (orig == nullptr || RowName(orig) != edit.was) {
			if (bank.version == 3) {
				Say("affix-edits.txt: %s is not '%s' in the game's table any more; that line does nothing", key.c_str(), edit.was.empty() ? "(an empty row)" : edit.was.c_str());
			}
		} else if (!PutAffix(bank, row, edit.cells, problem)) {
			Say("affix-edits.txt, data version %d, %s: %s", bank.version, key.c_str(), problem.c_str());
		} else {
			++done;
		}
	}
	if (!g_affixEdits.empty()) {
		LogF("data version %d: %d of %zu changed affix rows put back", bank.version, done, g_affixEdits.size());
	}
}

// ---- disabled affixes (affix-off.txt, one "prefix 9" a line): spawnable and frequency 0 in memory
// (affix::Off); the cells are not lost: they are the edited cells (affix-edits.txt) or the game's own.
std::set<std::string> g_affixOff;

std::wstring AffixOffPath() {
	return g_folder + L"\\affix-off.txt";
}
void ReadAffixOff() {
	g_affixOff.clear();
	std::string text;
	if (!ReadWhole(AffixOffPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (!line.empty() && line[0] != '#') {
			g_affixOff.insert(line);
		}
	}
}
void WriteAffixOff() {
	std::string text = "# Live Properties: the magic affixes disabled in the Affixes tab (spawnable and frequency 0 in memory: they roll on no drop,\r\n"
	                   "# and a recipe cannot force them). One a line, as the tab names them. Written by the plugin. Delete a line to enable it again.\r\n";
	for (const std::string& key : g_affixOff) {
		text += key + "\r\n";
	}
	MakeFolders(g_folder);
	if (!WriteOwn(AffixOffPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(AffixOffPath()).c_str());
	}
}
// The key's row in a bank ("prefix 9" -> its row), or false.
bool AffixKeyRow(const Bank& bank, const std::string& key, size_t& row) {
	for (int kind = 0; kind < 3; ++kind) {
		const std::string word = std::string(kAffixKinds[kind]) + " ";
		if (key.rfind(word, 0) == 0) {
			return AffixRowOf(bank, kind, static_cast<size_t>(strtoul(key.c_str() + word.size(), nullptr, 10)), row);
		}
	}
	return false;
}
void ApplyAffixOff(Bank& bank) {
	int done = 0;
	for (const std::string& key : g_affixOff) {
		size_t row = 0;
		if (AffixKeyRow(bank, key, row) && !affix::IsBlank(bank.affixes->rows + row * affix::kRow, bank.names)) {
			affix::Off(bank.affixes->rows + row * affix::kRow);
			++done;
		}
	}
	if (!g_affixOff.empty()) {
		LogF("data version %d: %d of %zu disabled affixes kept disabled", bank.version, done, g_affixOff.size());
	}
}

// ---- saved affix ids (memory d2r-saved-affix-ids): a saved prefix / suffix id is the row's place in
// its own kind + 1 (0 = none). Proven on the user's saves for every row after the files' Expansion
// line (the version column is 1 / 100 there, 0 / blank before it). The classic block before it is
// held back until `prop affixtest` has confirmed it in a save: affix-ids.txt "classic=confirmed".
bool g_classicIds = false; // the classic block's saved ids are confirmed (memory row + 1)
struct AffixTestState {
	bool   pending = false;
	int    kind    = kPrefixes;
	size_t index   = 0;
} g_affixTest;

std::wstring AffixIdsPath() {
	return g_folder + L"\\affix-ids.txt";
}
void ReadAffixIds() {
	g_classicIds   = false;
	g_affixTest    = {};
	std::string text;
	if (!ReadWhole(AffixIdsPath(), text)) {
		return;
	}
	g_classicIds = text.find("classic=confirmed") != std::string::npos;
	const size_t at = text.find("test=prefix ");
	if (at != std::string::npos) {
		g_affixTest.pending = true;
		g_affixTest.kind    = kPrefixes;
		g_affixTest.index   = static_cast<size_t>(strtoul(text.c_str() + at + 12, nullptr, 10));
	}
}
void WriteAffixIds() {
	std::string text = "# Live Properties: what is known about saved affix ids. Written by the plugin (prop affixtest).\r\n";
	if (g_classicIds) {
		text += "classic=confirmed\r\n";
	}
	if (g_affixTest.pending) {
		text += "test=prefix " + std::to_string(g_affixTest.index) + "\r\n";
	}
	MakeFolders(g_folder);
	static_cast<void>(WriteOwn(AffixIdsPath(), text));
}
// Where the classic block of a kind ends (its first row with version 1 or more), in its own numbering.
size_t ClassicEnd(const Bank& bank, int kind) {
	size_t first = 0, end = 0;
	if (!AffixRowOf(bank, kind, 0, first)) {
		return 0;
	}
	end = kind == kSuffixes ? bank.preBase : kind == kPrefixes ? bank.autoBase : bank.affixes->count;
	return affix::ClassicEnd(bank.affixes->rows + first * affix::kRow, end - first);
}
// Is the saved id of that row known (index + 1)? Why not, when it is not.
bool AffixIdKnown(const Bank& bank, int kind, size_t index, std::string& why) {
	if (kind == kAutomagic || g_classicIds || index >= ClassicEnd(bank, kind)) {
		return true;
	}
	why = AffixKey(kind, index) + " is in the classic block (before the files' Expansion line), where the saved id of an affix is not confirmed yet. Type `prop affixtest` in a game "
	      "(it makes a test prefix on a ring), save and quit to the menu, and start again: then the log says whether it is confirmed, and New / Remove work there.";
	return false;
}

// Remove of a made affix: refused while a saved item carries its id (Save Scan 0.3.3 scanAffixes). "" = clear.
std::string AffixHolders(int kind, size_t index) {
	const savescan::Api* scan = savescan::Find(2);
	if (scan == nullptr || scan->size < offsetof(savescan::Api, scanAffixes) + sizeof(void*) || scan->scanAffixes == nullptr) {
		return "Save Scan 0.3.3 or newer is not installed, so no save could be checked for this affix.";
	}
	const savescan::AffixId id { static_cast<uint8_t>(kind == kSuffixes ? 1 : 0), static_cast<uint16_t>(index + 1) };
	savescan::Summary       sum { sizeof(sum), 0, 0, 0, 0, 0 };
	struct Found {
		std::string text;
		int         n = 0;
	} found;
	static_cast<void>(scan->scanAffixes(&id, 1, &sum, [](void* u, const savescan::AffixHit* h) noexcept {
		auto* f = static_cast<Found*>(u);
		if (h != nullptr && f->n++ < 4) {
			f->text += std::string(f->text.empty() ? "" : "; ") + (h->file != nullptr ? h->file : "?") + " (" + (h->code != nullptr ? h->code : "?") + ", " + (h->where != nullptr ? h->where : "?") + ")";
		}
	}, nullptr, &found));
	LogF("affix %s (saved id %u): scan ran %u, %u files, %u unreadable, %u items carry it. %s", AffixKey(kind, index).c_str(), id.id, sum.ran, sum.files, sum.unreadable, sum.holders, found.text.c_str());
	if (sum.ran == 0) {
		return "the saves could not be read, so this affix stays.";
	}
	if (sum.unreadable != 0) {
		return std::to_string(sum.unreadable) + " save files could not be read to the end, so this affix stays.";
	}
	if (sum.holders != 0) {
		return std::to_string(sum.holders) + (sum.holders == 1 ? " saved item carries" : " saved items carry") + " it: " + found.text + (sum.holders > 4 ? " ..." : "") + ". It stays while they exist.";
	}
	return {};
}

// `prop affixtest` (game thread, in a game): a test prefix in the first empty classic-block prefix row,
// on a magic ring in the inventory; the next start reads the save and confirms the id or not.
void AffixIdTest() {
	Bank* main = MainBank();
	if (main == nullptr || !main->affixOn) {
		Say("affix test: the game's tables are not there");
		return;
	}
	if (g_items == nullptr || g_inventory == nullptr) {
		Say("affix test: this D2RLoader has no item service");
		return;
	}
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (g_inventory->getLocalPlayer(g_ctx, &player) != D2RL::Inventory::Result::Success || player == D2RL::InvalidPlayerHandle) {
		Say("affix test: enter a game first (a throwaway character is best)");
		return;
	}
	const size_t end = ClassicEnd(*main, kPrefixes);
	size_t       row = 0, index = 1;
	bool         found = false;
	for (; !found && index < end && AffixRowOf(*main, kPrefixes, index, row); ++index) {
		found = affix::IsBlank(main->affixes->rows + row * affix::kRow, main->names) && g_affixEdits.count(AffixKey(kPrefixes, index)) == 0;
	}
	--index;
	if (!found) {
		Say("affix test: no empty prefix row in the classic block (nothing to test)");
		return;
	}
	affix::Cells cells {};
	cells[affix::kNameCell]                  = "LP id test";
	cells[affix::kSpawnCell]                 = "1";
	cells[affix::kLevelCell]                 = "1";
	cells[affix::kFrequencyCell]             = "1";
	cells[affix::kModCell]                   = "ac";
	cells[affix::kModCell + 2]               = "1";
	cells[affix::kModCell + 3]               = "1";
	cells[affix::kTypeCell]                  = "ring";
	std::string problem;
	for (Bank& bank : g_banks) {
		size_t at = 0;
		if (bank.on && bank.affixOn && AffixRowOf(bank, kPrefixes, index, at) && affix::IsBlank(bank.affixes->rows + at * affix::kRow, bank.names) && !PutAffix(bank, at, cells, problem)) {
			Say("affix test: data version %d: %s", bank.version, problem.c_str());
			return;
		}
	}
	AffixEdit& edit = g_affixEdits[AffixKey(kPrefixes, index)];
	edit.kind = kPrefixes, edit.index = index, edit.was = "", edit.cells = AffixCells(*main, row);
	WriteAffixEdits();
	D2RL::Items::ItemCreateSpec spec {};
	spec.structSize        = D2RL::Items::ItemCreateSpecSize;
	spec.code              = D2RL::Items::MakeItemCode("rin");
	spec.quality           = D2RL::Items::Quality::Magic;
	spec.qualityRecordId   = D2RL::Items::RandomQualityRecord;
	spec.itemLevel         = 1;
	spec.prefixIds[0]      = static_cast<uint32_t>(main->preBase + index + 1); // the SDK's combined one-based id: suffixes first
	spec.seedMode          = D2RL::Items::SeedMode::Random;
	spec.quantity          = D2RL::Items::DefaultValue;
	spec.durability        = D2RL::Items::DefaultValue;
	spec.destination.structSize = D2RL::Items::ItemDestinationSize;
	spec.destination.container  = D2RL::Items::ItemContainer::Inventory;
	D2RL::ItemHandle item {};
	const auto              made = g_items->createItem(g_ctx, player, &spec, &item);
	if (made != D2RL::Items::Result::Success) {
		Say("affix test: the ring could not be made (result %u)", static_cast<unsigned>(made));
		return;
	}
	g_affixTest = { true, kPrefixes, index };
	WriteAffixIds();
	Say("affix test: a magic ring with the test prefix '%s' (prefix row %zu) is in your inventory. Save and quit to the menu, then start the game again: the log says whether its saved id is %zu (row + 1).",
		"LP id test", index, index + 1);
	LogF("affix test: prefix row %zu made 'LP id test' and a ring with it; expected saved id %zu", index, index + 1);
}

// At a start: the test ring's saved id read back (Save Scan), the switch turned on when it is row + 1.
void AffixIdCheck() {
	if (!g_affixTest.pending) {
		return;
	}
	const savescan::Api* scan = savescan::Find(2);
	if (scan == nullptr || scan->size < offsetof(savescan::Api, scanAffixes) + sizeof(void*) || scan->scanAffixes == nullptr) {
		LogF("affix test: Save Scan 0.3.3 or newer is needed to read the test ring back");
		return;
	}
	const size_t            k = g_affixTest.index;
	const savescan::AffixId ids[] = { { 0, static_cast<uint16_t>(k + 1) }, { 0, static_cast<uint16_t>(k) } };
	struct Seen {
		bool plusOne = false, same = false;
	} seen;
	struct Ctx {
		Seen*    seen;
		uint16_t plus;
	} ctx { &seen, static_cast<uint16_t>(k + 1) };
	savescan::Summary sum { sizeof(sum), 0, 0, 0, 0, 0 };
	static_cast<void>(scan->scanAffixes(ids, 2, &sum, [](void* u, const savescan::AffixHit* h) noexcept {
		auto* c = static_cast<Ctx*>(u);
		if (h != nullptr && h->code != nullptr && strncmp(h->code, "rin", 3) == 0 && h->suffix == 0) {
			(h->id == c->plus ? c->seen->plusOne : c->seen->same) = true;
		}
	}, nullptr, &ctx));
	if (seen.plusOne && !seen.same) {
		g_classicIds        = true;
		g_affixTest.pending = false;
		WriteAffixIds();
		LogF("affix test: saved id = row + 1: confirmed (prefix row %zu saved as %zu). New and Remove now work in the classic block too.", k, k + 1);
	} else if (seen.same && !seen.plusOne) {
		g_affixTest.pending = false;
		WriteAffixIds();
		LogF("affix test: NOT confirmed - prefix row %zu was saved as %zu (the row itself). The classic block stays refused; tell the Live Properties session.", k, k);
	} else {
		LogF("affix test: the test ring was not found in the saves yet (%u files read). Save and quit with it, then start again.", sum.files);
	}
}

// ---------------------------------------------------------------- our own rows in the tables' text
//
// ---------------------------------------------------------------- Live Tables: rows added during play
//
// New stat and New property ask Live Tables (../Live Tables/live_tables_api.h) for a row: it adds
// it at once, in the game's own memory, at a safe point, and lays it in again at every start
// (live-suite\live-tables\<mod>\grown-rows.txt). Its change listener only records the change in a
// lock-free slot (ltc::ChangeSlot); every Guard entry puts it into the banks first (ApplyChanges).
// A grow is never started under the lock: done() may come at once when Live Tables is off.
// Without Live Tables this plugin is an editor of the rows that are there.

std::atomic<const livetables::Api*> g_lt { nullptr };
std::atomic<int>                    g_ltLimit[2] { { -1 }, { -1 } }; // Live Tables' limit() per table (fixed for a run), so no other code asks it on a foreign thread
ltc::ChangeSlot                     g_slot;
ltc::GrowQueue                      g_growQueue;    // New stat / New property / the handover: one at a time
std::vector<ltc::GrowQueue::Item>   g_toGrow;       // under the lock: started by StartGrows, outside it
std::string                         g_toGrowWhat;   // "new" or "handover"
std::string                         g_handoverNote; // what the handover did, for the Stats list
std::string                         g_movedAlarm;   // made stats whose row number moved at this start (on screen, top of the Stats list)

void __cdecl OnTableChange(void*, const livetables::Change* change) noexcept {
	g_slot.Record(change); // at Live Tables' safe point: no lock, no allocation
}

// Live Tables, once it is loaded (it may load after this plugin). Not from Live Tables' callbacks.
const livetables::Api* LT() {
	const livetables::Api* lt = g_lt.load();
	if (lt == nullptr && (lt = livetables::Find()) != nullptr) {
		const livetables::Api* none = nullptr;
		if (g_lt.compare_exchange_strong(none, lt)) {
			g_ltLimit[0].store(lt->limit(livetables::Table::ItemStatCost, 0));
			g_ltLimit[1].store(lt->limit(livetables::Table::Properties, 0));
			const int handle = lt->addListener(OnTableChange, nullptr);
			LogF("Live Tables: found (API %u); change listener %s", lt->version, handle > 0 ? "on" : "REFUSED - grown rows show only after the next table read");
		}
	}
	return g_lt.load();
}

std::set<uint16_t> g_statOff;            // the main data version's disabled stats (Live Tables' isDisabled), for the marks
bool               g_disabledDirty = false; // a Disable / Enable came: read isDisabled again on the game / UI thread

std::string BackUpStatFiles(std::string& kept, const wchar_t* why);
void        WriteStatEdits();

// One change into one bank: the rows pointer, the count, the originals carried over by row number
// (ltc::Carry; after a middle-row Remove the stat numbers inside the originals follow the map too),
// the names walked again. False: nothing to do (already done, or the bank is in another state).
bool CarryBank(Bank& bank, const ltc::Record& r) {
	const bool     stats = r.table == livetables::Table::ItemStatCost;
	const size_t   size  = stats ? prop::kStatRow : prop::kPropRow;
	Vector*        v     = stats ? bank.stats : bank.props;
	auto&          orig  = stats ? bank.statOrig : bank.propOrig;
	const uint8_t* had   = stats ? bank.statOrigOf : bank.propRows;
	if (had == r.newRows && orig.size() == r.newCount * size) {
		return false; // already done (data version 2 follows 1's change, then gets its own)
	}
	if (had != r.oldRows || v->count != r.newCount || !Readable(v->rows, v->count * size) || !ltc::Carry(orig, size, r, v->rows, v->count)) {
		LogF("data version %d: a %s change of Live Tables did not fit what this plugin saw (%llu -> %llu rows); its originals are taken anew", bank.version, stats ? "stat" : "property",
			static_cast<unsigned long long>(r.oldCount), static_cast<unsigned long long>(r.newCount));
		orig.assign(v->rows, v->rows + v->count * size);
	}
	if (stats) {
		bank.statOrigOf = v->rows;
		if (r.kind == livetables::ChangeKind::Remove) { // the stat numbers the originals hold
			ltc::RemapStatWords(bank.statOrig, prop::kStatRow, { prop::kOpBase, prop::kOpStat, prop::kOpStat + 2, prop::kOpStat + 4 }, r.map, r.mapSize);
			std::vector<size_t> lines;
			for (size_t i = 0; i < 7; ++i) {
				lines.push_back(kPropStat + i * 2);
			}
			ltc::RemapStatWords(bank.propOrig, prop::kPropRow, lines, r.map, r.mapSize);
		}
		WalkNames(static_cast<const uint8_t*>(bank.tables), kStatNamesAt, v->count, bank.statNames);
	} else {
		bank.propRows = v->rows;
		bank.propBase = v->count;
		WalkNames(static_cast<const uint8_t*>(bank.tables), kPropNamesAt, v->count, bank.propNames);
	}
	static const char* const kinds[] = { "grew", "was read", "", "", "lost its last row", "lost a row" };
	LogF("data version %d: the %s table %s: %llu rows (Live Tables)", bank.version, stats ? "stat" : "property", kinds[static_cast<int>(r.kind)], static_cast<unsigned long long>(v->count));
	return true;
}

// A stat row removed: stat-edits.txt (keyed by number) follows Live Tables' map, after a dated backup.
void RenumberStatFiles(const ltc::Record& r) {
	std::vector<uint16_t> dropped;
	const auto            next = ltc::RemapKeys(g_statEdits, r.map, r.mapSize, dropped);
	if (next.size() == g_statEdits.size() && dropped.empty() && std::equal(next.begin(), next.end(), g_statEdits.begin(), [](const auto& a, const auto& b) { return a.first == b.first; })) {
		return; // no line is at or past the removed row
	}
	std::string kept;
	if (const std::string why = BackUpStatFiles(kept, L"before-remove"); !why.empty()) {
		LogF("stat %llu removed: %s - stat-edits.txt is renumbered in memory only this session", static_cast<unsigned long long>(r.number), why.c_str());
	}
	g_statEdits = next;
	WriteStatEdits();
	std::string said;
	for (const uint16_t d : dropped) {
		said += (said.empty() ? "" : ", ") + std::to_string(d);
	}
	LogF("stat %llu removed: stat-edits.txt follows the table (%s%s; backup: %s)", static_cast<unsigned long long>(r.number), dropped.empty() ? "no line dropped" : "line dropped: ", said.c_str(),
		kept.empty() ? "none needed" : kept.c_str());
}

void WriteEdits();
void WriteMade();
void WriteOff();
void WriteAffixEdits();
std::wstring EditsPath();
std::wstring MadePath();
std::wstring OffPath();
std::wstring AffixEditsPath();

// Dated copies of some of this plugin's files ("" = done, else why not); `kept` lists them.
std::string BackUpFiles(const std::vector<std::wstring>& files, const wchar_t* why, std::string& kept) {
	SYSTEMTIME now;
	GetLocalTime(&now);
	wchar_t stamp[32];
	swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	for (const std::wstring& from : files) {
		if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
			continue;
		}
		const size_t       dot = from.rfind(L'.');
		const std::wstring to  = (dot == std::wstring::npos ? from : from.substr(0, dot)) + L"." + why + L"-" + stamp + L".txt";
		if (!CopyFileW(from.c_str(), to.c_str(), TRUE)) {
			return Narrow(from) + " could not be backed up";
		}
		kept += (kept.empty() ? "" : ", ") + Narrow(to);
	}
	return {};
}

// A row Live Tables renamed: every edit that names it follows (dated backups first). Stats: the
// made stat's line, op base / op stat cells of other stat lines, the stat cells of property lines.
// Properties: property-edits.txt, made-properties.txt, property-off.txt keys, affix mod codes.
void RenameInFiles(bool stats, const std::string& from, const std::string& to) {
	bool statFile = false, editFile = false, madeFile = false, offFile = false, affixFile = false, madeStatsFile = false;
	if (stats) {
		for (auto& [number, cells] : g_statEdits) {
			for (int i = 0; i < kNameCell; ++i) {
				statFile = (kFields[i].kind == kCellStat && ltc::RenameCell(cells[static_cast<size_t>(i)], from, to)) || statFile;
			}
			statFile = ltc::RenameCell(cells[kNameCell], from, to) || statFile;
			statFile = ltc::RenameCell(cells[kWasCell], from, to) || statFile;
		}
		if (g_madeStats.erase(from) != 0) {
			g_madeStats.insert(to);
			madeStatsFile = true; // written after the dated backup below
		}
		for (auto& [key, cells] : g_edits) {
			for (int line = 0; line < 7; ++line) {
				editFile = ltc::RenameCell(cells[static_cast<size_t>(line * 4 + 1)], from, to) || editFile;
			}
		}
	} else {
		editFile = ltc::RenameKey(g_edits, from, to) != 0;
		for (Made& made : g_made) {
			madeFile = ltc::RenameCell(made.code, from, to) || madeFile;
			madeFile = ltc::RenameCell(made.was, from, to) || madeFile;
		}
		if (g_off.erase(from) != 0) {
			g_off.insert(to);
			offFile = true;
		}
		for (auto& [key, edit] : g_affixEdits) {
			for (int m = 0; m < 3; ++m) {
				affixFile = ltc::RenameCell(edit.cells[static_cast<size_t>(affix::kModCell + m * 4)], from, to) || affixFile;
			}
		}
	}
	std::vector<std::wstring> files;
	if (statFile) files.push_back(StatEditsPath());
	if (editFile) files.push_back(EditsPath());
	if (madeFile) files.push_back(MadePath());
	if (offFile) files.push_back(OffPath());
	if (affixFile) files.push_back(AffixEditsPath());
	if (madeStatsFile) files.push_back(MadeStatsPath());
	if (files.empty()) {
		LogF("%s '%s' is now '%s' (Live Tables); no edit file of this plugin names it", stats ? "stat" : "property", from.c_str(), to.c_str());
		return;
	}
	std::string kept;
	if (const std::string why = BackUpFiles(files, L"before-rename", kept); !why.empty()) {
		LogF("%s '%s' is now '%s' (Live Tables): %s - the files are renamed in memory only this session", stats ? "stat" : "property", from.c_str(), to.c_str(), why.c_str());
		return;
	}
	if (statFile) WriteStatEdits();
	if (editFile) WriteEdits();
	if (madeFile) WriteMade();
	if (offFile) WriteOff();
	if (affixFile) WriteAffixEdits();
	if (madeStatsFile) WriteMadeStats();
	LogF("%s '%s' is now '%s' (Live Tables): %zu edit files follow (backups: %s)", stats ? "stat" : "property", from.c_str(), to.c_str(), files.size(), kept.c_str());
}

// Under the lock (every Guard entry): what Live Tables changed since the last entry, in order.
void ApplyChanges() {
	if (g_slot.Empty()) {
		return;
	}
	static ltc::Record recs[ltc::ChangeSlot::kRoom]; // under the lock: one user
	bool               lost = false;
	const size_t       n    = g_slot.Drain(recs, ltc::ChangeSlot::kRoom, lost);
	const Bank*        main = MainBank();
	bool               any  = false;
	for (size_t i = 0; i < n; ++i) {
		const ltc::Record& r = recs[i];
		if (r.kind == livetables::ChangeKind::Load) {
			continue; // the plugin reads loaded tables itself (DataTablesLoaded)
		}
		if (r.kind == livetables::ChangeKind::CellEdit) { // a made row compiled again (setCells): it is now its own original
			for (Bank& bank : g_banks) {
				if (!bank.on || !(bank.version == r.dataVersion || (r.dataVersion == 1 && bank.version == 2))) {
					continue;
				}
				const bool     stats = r.table == livetables::Table::ItemStatCost;
				const size_t   size  = stats ? prop::kStatRow : prop::kPropRow;
				Vector*        v     = stats ? bank.stats : bank.props;
				auto&          orig  = stats ? bank.statOrig : bank.propOrig;
				if (r.number < v->count && (r.number + 1) * size <= orig.size() && Readable(v->rows + r.number * size, size)) {
					memcpy(orig.data() + r.number * size, v->rows + r.number * size, size);
				}
			}
			any = true;
			continue;
		}
		if (r.kind == livetables::ChangeKind::Alarm) { // Live Tables' alarm went up or down: the lists' first lines say it
			any = true;
			continue;
		}
		if (r.kind == livetables::ChangeKind::Disable || r.kind == livetables::ChangeKind::Enable) {
			g_disabledDirty = true; // the row changed in place: nothing moves; the marks are read again
			any             = true;
			continue;
		}
		if (r.kind == livetables::ChangeKind::Rename) { // no row byte changed: the names, and the edit files that name it
			const bool stats = r.table == livetables::Table::ItemStatCost;
			for (Bank& bank : g_banks) {
				if (bank.on && (bank.version == r.dataVersion || (r.dataVersion == 1 && bank.version == 2))) {
					if (stats) {
						WalkNames(static_cast<const uint8_t*>(bank.tables), kStatNamesAt, bank.stats->count, bank.statNames);
					} else {
						WalkNames(static_cast<const uint8_t*>(bank.tables), kPropNamesAt, bank.propBase, bank.propNames);
					}
				}
			}
			if (main != nullptr && r.dataVersion == main->version && r.oldName[0] != '\0' && r.newName[0] != '\0') {
				RenameInFiles(stats, r.oldName, r.newName);
			}
			any = true;
			continue;
		}
		for (Bank& bank : g_banks) {
			if (bank.on && (bank.version == r.dataVersion || (r.dataVersion == 1 && bank.version == 2))) {
				any = CarryBank(bank, r) || any;
			}
		}
		if (r.table == livetables::Table::ItemStatCost && (r.kind == livetables::ChangeKind::Shrink || r.kind == livetables::ChangeKind::Remove) && main != nullptr && r.dataVersion == main->version
			&& r.map != nullptr) {
			RenumberStatFiles(r);
		}
	}
	if (lost) { // more changes than the queue holds: take the tables as they are
		LogF("more Live Tables changes than this plugin could keep: originals and names are taken as the tables are now");
		for (Bank& bank : g_banks) {
			if (bank.on) {
				bank.statOrig.assign(bank.stats->rows, bank.stats->rows + bank.stats->count * prop::kStatRow);
				bank.statOrigOf = bank.stats->rows;
				bank.propOrig.assign(bank.props->rows, bank.props->rows + bank.props->count * prop::kPropRow);
				bank.propRows = bank.props->rows;
				bank.propBase = bank.props->count;
				WalkNames(static_cast<const uint8_t*>(bank.tables), kStatNamesAt, bank.stats->count, bank.statNames);
				WalkNames(static_cast<const uint8_t*>(bank.tables), kPropNamesAt, bank.props->count, bank.propNames);
			}
		}
		any = true;
	}
	if (any) {
		BuildViews();
	}
}

// Live Tables' disabled marks of the main data version. Game or UI thread only (isDisabled reads
// Live Tables' tables), under the lock. True when they changed.
bool RefreshDisabled() {
	const livetables::Api* lt   = g_lt.load();
	const Bank*            main = MainBank();
	std::set<uint16_t>     off;
	if (lt != nullptr && lt->version >= 2 && lt->isDisabled != nullptr && main != nullptr) {
		for (size_t i = 0; i < main->stats->count; ++i) {
			if (lt->isDisabled(livetables::Table::ItemStatCost, main->version, static_cast<int>(i)) == 1) {
				off.insert(static_cast<uint16_t>(i));
			}
		}
	}
	g_disabledDirty = false;
	if (off == g_statOff) {
		return false;
	}
	g_statOff.swap(off);
	return true;
}
bool LtV2() {
	const livetables::Api* lt = g_lt.load();
	return lt != nullptr && lt->version >= 2 && lt->size >= sizeof(livetables::Api);
}

bool LtV4() {
	const livetables::Api* lt = g_lt.load();
	return lt != nullptr && lt->version >= 4 && lt->size >= offsetof(livetables::Api, moved) + sizeof(lt->moved);
}

// Is the stat disabled in that bank's data version (Live Tables)? Game / UI thread.
bool StatDisabledIn(const Bank& bank, uint16_t stat) {
	const livetables::Api* lt = g_lt.load();
	return LtV2() && lt->isDisabled(livetables::Table::ItemStatCost, bank.version, stat) == 1;
}

bool StatOff(uint16_t stat) {
	return g_statOff.count(stat) != 0;
}

// "389 of 511 used" for a tab (empty: Live Tables is not there). Under the lock, any thread: the
// count is the bank's (kept current by ApplyChanges), the limit the one Live Tables gave at the start.
std::string UsedText(livetables::Table table) {
	const Bank* main = MainBank();
	if (g_lt.load() == nullptr || main == nullptr) {
		return {};
	}
	const bool stats = table == livetables::Table::ItemStatCost;
	return ltc::Used(static_cast<int>(stats ? main->stats->count : main->propBase), g_ltLimit[stats ? 0 : 1].load());
}

// A made stat's line moves to the number its row has. Under the lock.
void MoveLine(size_t from, size_t to) {
	auto line = g_statEdits.find(static_cast<uint16_t>(from));
	if (line == g_statEdits.end() || from == to) {
		return;
	}
	StatCells cells = line->second;
	cells[kWasCell] = cells[kNameCell]; // the row is called by the made name
	g_statEdits.erase(line);
	g_statEdits[static_cast<uint16_t>(to)] = cells;
}

// Dated copies of stat-edits.txt and stat-ids.txt before they change ("" = copied, else why not).
std::string BackUpStatFiles(std::string& kept, const wchar_t* why) {
	SYSTEMTIME now;
	GetLocalTime(&now);
	wchar_t stamp[32];
	swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	for (const wchar_t* file : { L"stat-edits", L"stat-ids" }) {
		const std::wstring from = g_folder + L"\\" + file + L".txt", to = g_folder + L"\\" + file + L"." + why + L"-" + stamp + L".txt";
		if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
			continue;
		}
		if (!CopyFileW(from.c_str(), to.c_str(), TRUE)) {
			return Narrow(from) + " could not be backed up";
		}
		kept += (kept.empty() ? "" : ", ") + Narrow(to);
	}
	return {};
}

// After the tables are read (under the lock): made stats whose row is not in the table (477 / 478,
// made on the older Exalted Items' filler rows) are handed to Live Tables once, so they get the
// numbers after the table's end and keep them; a line whose name the table has at another number
// follows the table. Save Scan must find no saved item with an old or a new number first.
void HandOver() {
	Bank*                  main = MainBank();
	const livetables::Api* lt   = LT();
	if (main == nullptr || lt == nullptr || g_growQueue.Busy() || !g_toGrow.empty()) {
		return;
	}
	std::vector<ltc::Line> lines;
	for (const auto& [number, cells] : g_statEdits) {
		if (!cells[kNameCell].empty()) {
			lines.push_back({ number, cells[kNameCell], cells[kWasCell] });
		}
	}
	if (lines.empty()) {
		return;
	}
	auto find = [&](const std::string& name) { return ltc::IndexOf(main->statNames, name); };
	const int           limit = g_ltLimit[0].load();
	const size_t        most  = limit > 0 ? std::min<size_t>(static_cast<size_t>(limit) - 1, kStatMost) : kStatMost;
	const ltc::Handover plan  = ltc::PlanHandover(lines, find, main->stats->count, most);
	if (plan.grow.empty() && plan.renumber.empty()) {
		return;
	}
	for (const std::string& note : plan.notes) {
		LogF("handover: %s", note.c_str());
	}
	std::string kept;
	if (const std::string why = BackUpStatFiles(kept, L"before-handover"); !why.empty()) {
		LogF("handover: NOT done - %s. stat-edits.txt stays as it is.", why.c_str());
		return;
	}
	if (!plan.renumber.empty()) {
		std::string said;
		for (const auto& [from, to] : plan.renumber) {
			MoveLine(from, to);
			said += (said.empty() ? "" : ", ") + std::to_string(from) + " -> " + std::to_string(to);
		}
		WriteStatEdits();
		for (Bank& bank : g_banks) {
			if (bank.on) {
				ApplyStatEdits(bank);
			}
		}
		g_movedAlarm = said;
		LogF("WARNING: made stats have new numbers this start (%s): saved items made with them now read as other stats. stat-edits.txt follows the table (backups: %s)", said.c_str(), kept.c_str());
	}
	if (plan.grow.empty()) {
		return;
	}
	std::string why;
	const savescan::Api* scan = savescan::Find();
	savescan::Summary    sum { sizeof(sum), 0, 0, 0, 0, 0 };
	ScanText             found;
	if (scan == nullptr) {
		why = "the Save Scan plugin is not installed, so no save could be checked for the numbers";
	} else {
		const int ran = scan->scan(plan.scan.data(), static_cast<uint32_t>(plan.scan.size()), &sum, OnScanHit, OnScanProblem, &found);
		std::string list;
		for (const uint16_t n : plan.scan) {
			list += (list.empty() ? "" : " ") + std::to_string(n);
		}
		LogF("handover: save scan for stats %s: ran %u, %u files, %u unreadable, %u items, %u hold one. %s %s", list.c_str(), sum.ran, sum.files, sum.unreadable, sum.items, sum.holders, found.hits.c_str(), found.problems.c_str());
		if (ran == 0 || !savescan::Safe(sum)) {
			why = sum.ran == 0 ? "the save scan could not run" : sum.unreadable != 0 ? std::to_string(sum.unreadable) + " save files could not be read to the end" : std::to_string(sum.holders) + " saved items hold one of the numbers";
		}
	}
	if (!why.empty()) {
		g_handoverNote = "Made stats without a row (" + std::to_string(plan.grow.size()) + ") were not handed to Live Tables: " + why + ".";
		LogF("handover: NOT done - %s. Their lines stay as they are and do nothing this session.", why.c_str());
		return;
	}
	for (const ltc::Line& line : plan.grow) {
		const StatCells& cells = g_statEdits[static_cast<uint16_t>(line.number)];
		g_toGrow.push_back({ livetables::Table::ItemStatCost, ltc::MadeStatRow(line.name, std::vector<std::string>(cells.begin(), cells.begin() + 5)), line.name, line.number });
	}
	g_toGrowWhat = "handover";
	LogF("handover: %zu made stats go to Live Tables (backups: %s)", plan.grow.size(), kept.empty() ? "none needed" : kept.c_str());
}

void StartGrows();

void MigrateMadeStats();
void ReadStatAdv();
void ApplyStatAdv(Bank& bank);

void SetUp() {
	g_setUp  = true;
	g_folder = OwnFolder();
	if (!g_moved.empty()) {
		LogF("your files were %s", g_moved.c_str());
		g_moved.clear();
	}
	ReadEdits();
	ReadOff();
	ReadMade();
	ReadStatEdits();
	ReadMadeStats();
	ReadStatAdv();
	ReadAffixEdits();
	ReadAffixOff();
	ReadAffixIds();
	{
		std::string keys;
		if (ReadWhole(g_folder + L"\\string-keys.txt", keys)) {
			for (size_t at = 0; at < keys.size();) {
				size_t end = keys.find('\n', at);
				end        = end == std::string::npos ? keys.size() : end;
				std::string line = keys.substr(at, end - at);
				at               = end + 1;
				const size_t tab = line.find('\t');
				if (!line.empty() && line[0] != '#' && tab != std::string::npos) {
					while (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					g_typedKeys[static_cast<uint16_t>(atoi(line.c_str()))] = line.substr(tab + 1);
				}
			}
		}
	}
	if (!CheckPrints()) {
		return;
	}
	FindStrings();
	for (int i = 0; i < 3; ++i) {
		Bank& bank   = g_banks[i];
		bank.version = static_cast<uint8_t>(i + 1);
		bank.on      = false;
		bank.off.clear();
		auto* tables = static_cast<uint8_t*>(g_link.tables(bank.version));
		if (!Readable(tables, kStatsAt + sizeof(Vector))) {
			bank.off = "the game has no tables for it";
			continue;
		}
		bank.tables = tables;
		bank.stats  = reinterpret_cast<Vector*>(tables + kStatsAt);
		bank.props  = reinterpret_cast<Vector*>(tables + kPropsAt);
		if (bank.stats->count == 0 || bank.stats->count > 0x8000 || !Readable(bank.stats->rows, bank.stats->count * prop::kStatRow)) {
			bank.off = "its stat table cannot be read (" + std::to_string(bank.stats->count) + " rows)";
			continue;
		}
		// the rows as the game read them: taken when the table is a new one
		if (bank.statOrigOf != bank.stats->rows || bank.statOrig.size() != bank.stats->count * prop::kStatRow) {
			bank.statOrig.assign(bank.stats->rows, bank.stats->rows + bank.stats->count * prop::kStatRow);
			bank.statOrigOf = bank.stats->rows;
		}
		int seen = 0;
		if (!LayoutHolds(bank, seen)) {
			bank.off = "its stat table is not linked the way the plugin expects";
			continue;
		}
		if (!TakeProperties(bank)) {
			continue;
		}
		bank.on = true;
		WalkNames(tables, kStatNamesAt, bank.stats->count, bank.statNames);
		WalkNames(tables, kPropNamesAt, bank.propBase, bank.propNames);
		RefreshMade(bank); // a made property the game now reads under its own code
		ApplyEdits(bank);
		ApplyOff(bank);
		ApplyStatEdits(bank);
		ApplyStatAdv(bank);
		if (AffixSetUp(bank)) {
			ApplyAffixEdits(bank);
			ApplyAffixOff(bank);
		}
		LogF("data version %d: on - %d op stats checked, %llu stats, %llu properties", bank.version, seen, static_cast<unsigned long long>(bank.stats->count),
			static_cast<unsigned long long>(bank.propBase));
	}
	for (const Bank& bank : g_banks) {
		if (!bank.on) {
			LogF("data version %d: off - %s", bank.version, bank.off.c_str());
		}
	}
	AffixIdCheck();
	{ // changes recorded before this read are in the tables just read
		static ltc::Record drop[ltc::ChangeSlot::kRoom];
		bool               lost = false;
		while (g_slot.Drain(drop, ltc::ChangeSlot::kRoom, lost) != 0) {
		}
	}
	RefreshDisabled();
	HandOver();
	MigrateMadeStats();
	BuildViews();
}

void Run(const Command& command) {
	Bank* main = MainBank();
	if (main == nullptr) {
		Say("off - %s", !g_link.ok ? g_linkProblem.c_str() : g_banks[2].off.c_str());
		return;
	}
	if (command.kind == Command::Edit || command.kind == Command::Restore || command.kind == Command::PropOff || command.kind == Command::PropOn) {
		RunEdit(command);
	} else if (command.kind == Command::StatEdit || command.kind == Command::StatRestore || command.kind == Command::StatLine || command.kind == Command::StatRemoveAsk || command.kind == Command::StatGameView || command.kind == Command::StatAdv) {
		RunStat(command);
	} else if (command.kind == Command::AffixTest) {
		AffixIdTest();
	} else if (command.kind == Command::AffixEdit || command.kind == Command::AffixRestore || command.kind == Command::AffixOff || command.kind == Command::AffixOn) {
		RunAffix(command);
	}
}

// ---------------------------------------------------------------- the Mod Book tabs

const modbook::Api* g_book = nullptr;

struct Entry {
	std::string title, detail, word; // word: what a click puts into a cell
	int         row = 0;
};
struct ListView {
	int                 handle = 0;
	int                 side   = 0;
	std::vector<Entry>  entries;
	std::vector<size_t> found; // the entries that have all the search words
	std::string         words;
	size_t              top = 0;
	// its card (the Properties and the Stats tab)
	int         card = -1; // the row whose card is open; -1 = the list
	int         cell = -1; // the cell being typed in
	std::string cellText, note;
	std::map<int, std::string> pending; // cells typed (Enter / Tab) and not saved yet: Save changes sends them to the game
	bool                       asking = false;    // the Properties card asks: disable (or remove) this property?
	bool                       askRemove = false; // the question is about removing a made property
	std::string                uses;              // what names it (PropUses), for the question
	std::vector<int>           askProps;          // the stat card's Disable: the properties that name the stat
	bool                       gameView = false;  // the stat card shows "How the game sees it"
	std::string                gameText;          // that page, made on the game thread when it opens
	bool                       naming = false;    // the Properties tab asks for a new property's code
	std::string                nameText;
};
ListView g_statsView, g_propsView, g_funcsView, g_affixView;
bool     g_affixChoosing = false; // the Affixes tab asks: a new prefix or a new suffix?
constexpr size_t kListRows = 9;

// What each func does: the game's data guide's words (legend_core). Which funcs exist is read from the game.
const char* FuncWords(int func) {
	return legend::FuncMeaning(func);
}

std::string Cut(const std::string& text, size_t most) {
	return text.size() <= most ? text : text.substr(0, most - 3) + "...";
}

// One of the game's strings, by its number, for showing (colour codes and line ends taken out).
std::string StringText(uint16_t id) {
	if (id == 0 || g_link.stringById == nullptr) {
		return {};
	}
	const char* at = g_link.stringById(id);
	std::string text;
	for (size_t i = 0; i < 80 && at != nullptr; ++i) {
		if ((i == 0 || (reinterpret_cast<uintptr_t>(at + i) & 0xfff) == 0) && !Readable(at + i, 1)) {
			break;
		}
		const unsigned char c = static_cast<unsigned char>(at[i]);
		if (c == 0) {
			break;
		}
		text += c < 0x20 || c > 0x7e ? ' ' : static_cast<char>(c);
	}
	return text;
}

void Filter(ListView& view) {
	std::vector<std::string> words;
	std::string              word;
	for (const char letter : view.words + " ") {
		if (letter == ' ') {
			if (!word.empty()) {
				words.push_back(word);
			}
			word.clear();
		} else {
			word += static_cast<char>(tolower(static_cast<unsigned char>(letter)));
		}
	}
	view.found.clear();
	for (size_t i = 0; i < view.entries.size(); ++i) {
		std::string all = view.entries[i].title + " " + view.entries[i].detail;
		for (char& letter : all) {
			letter = static_cast<char>(tolower(static_cast<unsigned char>(letter)));
		}
		bool has = true;
		for (const std::string& w : words) {
			has = has && all.find(w) != std::string::npos;
		}
		if (has) {
			view.found.push_back(i);
		}
	}
	const size_t last = view.found.size() > kListRows ? view.found.size() - kListRows : 0;
	view.top          = view.top > last ? last : view.top;
}

// ---- the Legend's sections (Mod Book 0.8.0): built with the lists, read by Mod Book on its UI thread
enum LegendPart { kLegendProps, kLegendStats, kLegendAffixes, kLegendParts };
legend::Section   g_legend[kLegendParts];
int               g_legendHandle[kLegendParts] = {};
std::atomic<bool> g_legendDirty { false };

// "In your tables" examples: the first rows of the tables the game read that show each entry.
void BuildLegend(const Bank* bank) {
	legend::Examples props, stats, affixes;
	if (bank != nullptr) {
		// properties: the first row using each func, and rows that show a shared roll
		for (size_t i = 0; i < bank->props->count && bank->props->rows == bank->propRows; ++i) {
			const std::string key   = PropKey(*bank, i);
			const Cells       cells = RowCells(*bank, bank->propRows + i * prop::kPropRow);
			int               lines = 0;
			for (int line = 0; line < 7; ++line) {
				const std::string& func = cells[static_cast<size_t>(line * 4)];
				if (func.empty()) {
					continue;
				}
				++lines;
				const std::string said = key + " line " + std::to_string(line + 1) + ": func " + func + (cells[static_cast<size_t>(line * 4 + 1)].empty() ? "" : " " + cells[static_cast<size_t>(line * 4 + 1)])
				                       + (cells[static_cast<size_t>(line * 4 + 2)].empty() ? "" : " set " + cells[static_cast<size_t>(line * 4 + 2)])
				                       + (cells[static_cast<size_t>(line * 4 + 3)].empty() ? "" : " val " + cells[static_cast<size_t>(line * 4 + 3)]);
				props.emplace("func " + func, said);
				if (line > 0 && (func == "3" || func == "4") && !cells[static_cast<size_t>(line * 4 - 4)].empty()) {
					props.emplace("shared rolls", key + ": line " + std::to_string(line) + " func " + cells[static_cast<size_t>(line * 4 - 4)] + ", line " + std::to_string(line + 1) + " func " + func + " (the same roll)");
				}
				if (!cells[static_cast<size_t>(line * 4 + 2)].empty()) {
					props.emplace("set", said);
				}
				if (!cells[static_cast<size_t>(line * 4 + 3)].empty()) {
					props.emplace("val", said);
				}
			}
			if (lines >= 3) {
				props.emplace("a property", key + " has " + std::to_string(lines) + " lines (open it in the Properties tab)");
			}
		}
		// stats: a row for each column, op and descfunc; units, room and dependents
		const size_t count = bank->stats->count;
		size_t       mostTaken = 0, mostDep = 0;
		std::string  taken, dep, x256, plain;
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* row = bank->stats->rows + i * prop::kStatRow;
			if (prop::IsBlank(row)) {
				continue;
			}
			const std::string name  = StatWord(*bank, static_cast<uint16_t>(i));
			const StatCells   cells = StatRowCells(*bank, row);
			for (int c = 0; c < kNameCell; ++c) {
				if (!cells[static_cast<size_t>(c)].empty() && (kFields[c].group != kGroupOp || row[prop::kOpAt] != 0)) {
					stats.emplace(kFields[c].name, name + ": " + cells[static_cast<size_t>(c)]);
				}
			}
			if (row[prop::kOpAt] != 0) {
				uint16_t base, target;
				memcpy(&base, row + prop::kOpBase, 2);
				memcpy(&target, row + prop::kOpStat, 2);
				stats.emplace("op " + std::to_string(row[prop::kOpAt]),
					name + ": op " + std::to_string(row[prop::kOpAt]) + (base < count ? ", op base " + StatWord(*bank, base) : "") + (row[prop::kOpParam] != 0 ? ", op param " + std::to_string(row[prop::kOpParam]) : "")
						+ (target < count ? ", to " + StatWord(*bank, target) : ""));
			}
			if (row[prop::kDescFunc] != 0) {
				uint16_t pos;
				memcpy(&pos, row + prop::kDescStrPos, 2);
				const std::string words = StringText(pos);
				stats.emplace("descfunc " + std::to_string(row[prop::kDescFunc]), name + (words.empty() ? "" : ": \"" + Cut(words, 60) + "\""));
			}
			if (std::string& kept = row[prop::kValShift] == 8 ? x256 : plain; kept.size() < 40) {
				kept += (kept.empty() ? "" : ", ") + name;
			}
			size_t places = 0, dependents = 0;
			while (places < static_cast<size_t>(prop::kOpListMax) && row[prop::kOpList + places * 6 + 4] != 0) {
				++places;
			}
			for (; dependents < static_cast<size_t>(prop::kDependentMax); ++dependents) {
				uint16_t other;
				memcpy(&other, row + prop::kDependents + dependents * 2, 2);
				if (other >= count) {
					break;
				}
			}
			if (places > mostTaken) {
				mostTaken = places;
				taken     = name + ": " + std::to_string(places) + " of its 16 places are taken";
			}
			if (dependents > mostDep) {
				mostDep = dependents;
				dep     = name + ": " + std::to_string(dependents) + " stats are worked out from it";
			}
		}
		stats.emplace("kept as", "x256: " + x256 + "; plain: " + plain);
		stats.emplace("room", taken);
		stats.emplace("dep", dep);
		std::string made;
		for (const auto& [stat, cells] : g_statEdits) {
			if (!cells[kNameCell].empty()) {
				made += (made.empty() ? "" : ", ") + cells[kNameCell] + " (" + std::to_string(stat) + ")";
			}
		}
		stats.emplace("made stats", made.empty() ? "none made yet" : "made: " + made);
		// affixes: the first row of each kind that fills each column
		if (bank->affixOn) {
			for (size_t i = 0; i < bank->affixes->count; ++i) {
				const uint8_t* row = bank->affixes->rows + i * affix::kRow;
				if (affix::IsBlank(row, bank->names)) {
					continue;
				}
				int    kind  = 0;
				size_t index = 0;
				AffixPlace(*bank, i, kind, index);
				if (kind == kAutomagic) {
					continue;
				}
				const affix::Cells cells = AffixCells(*bank, i);
				const std::string  who   = (cells[affix::kNameCell].empty() ? std::string("(no name)") : cells[affix::kNameCell]) + " (" + (kind == kPrefixes ? "pre=" : "suf=") + std::to_string(index) + ")";
				if (kind == kPrefixes && !cells[affix::kNameCell].empty() && cells[affix::kSpawnCell] == "1") {
					affixes.emplace("pre= / suf=", "pre=" + std::to_string(index) + " is " + cells[affix::kNameCell] + " (cm1,mag,pre=" + std::to_string(index) + ")");
				}
				for (int c = 0; c < affix::kCells; ++c) {
					if (cells[static_cast<size_t>(c)].empty()) {
						continue;
					}
					const std::string field = affix::kFields[c].name;
					std::string       code  = field;
					if (c >= affix::kModCell && c < affix::kTypeCell) {
						static const char* const parts[] = { "mod code", "mod param", "mod min", "mod max" };
						code = parts[(c - affix::kModCell) % 4];
					} else if (c >= affix::kTypeCell) {
						code = field.rfind("itype", 0) == 0 ? "itype" : "etype";
					}
					affixes.emplace(code, who + ": " + field + " " + cells[static_cast<size_t>(c)]);
				}
			}
		}
	}
	g_legend[kLegendProps]   = legend::Properties(FuncCount(), FuncWorks, props);
	g_legend[kLegendStats]   = legend::Stats(stats);
	g_legend[kLegendAffixes] = legend::Affixes(affixes);
	g_legendDirty.store(true);
}

// Mod Book asks for the entries on its UI thread; the texts are copied into a buffer of this
// thread, so they live past the lock until the next ask.
int __cdecl LegendCount(void* user, int how) noexcept {
	Guard       guard;
	const auto* s = static_cast<const legend::Section*>(user);
	return static_cast<int>((how != 0 ? s->how : s->list).size());
}
int __cdecl LegendGet(void* user, int how, int index, modbook::LegendEntry* entry) noexcept {
	thread_local legend::Item copy;
	{
		Guard       guard;
		const auto* s    = static_cast<const legend::Section*>(user);
		const auto& list = how != 0 ? s->how : s->list;
		if (entry == nullptr || index < 0 || static_cast<size_t>(index) >= list.size()) {
			return 0;
		}
		copy = list[static_cast<size_t>(index)];
	}
	entry->code    = copy.code.c_str();
	entry->meaning = copy.meaning.c_str();
	entry->detail  = copy.detail.c_str();
	entry->insert  = copy.insert.c_str();
	return 1;
}

// Reads the lists from the tables as they are now. Under the lock.
void BuildViews() {
	g_statsView.entries.clear();
	g_propsView.entries.clear();
	g_funcsView.entries.clear();
	g_affixView.entries.clear();
	for (int func = 0; func < FuncCount(); ++func) {
		Entry entry;
		entry.row    = func;
		entry.word   = std::to_string(func);
		entry.title  = "func " + std::to_string(func) + (func == 0 || FuncWorks(func) ? "" : "   (does nothing)");
		entry.detail = func == 0 ? "the line does nothing (an empty func cell)" : FuncWorks(func) ? FuncWords(func) : "the game has no handler for this number: a line with it is skipped";
		g_funcsView.entries.push_back(std::move(entry));
	}
	const Bank* bank = MainBank();
	if (bank != nullptr) {
		const size_t count = bank->stats->count;
		if (LtV4()) { // Live Tables' alarm (any thread): a made stat whose number moved at this start
			char text[512] = "";
			if (g_lt.load()->status(text, sizeof(text)) == 1 && text[0] != '\0') {
				Entry entry;
				entry.row    = -1;
				entry.title  = "WARNING (Live Tables): " + Cut(text, 80);
				entry.detail = std::string(text) + " At the main menu Live Tables can renumber the saves to this start's numbers (dated backup); in a game it refuses. Until then every stat change is refused.";
				g_statsView.entries.push_back(std::move(entry));
			}
		} else if (!g_movedAlarm.empty()) { // no Live Tables 0.3.2: this plugin's own view of it
			Entry entry;
			entry.row    = -1;
			entry.title  = "WARNING: made stats have new numbers (" + g_movedAlarm + ")";
			entry.detail = "A row added or removed above them in the mod's itemstatcost.txt moved them. Saved items made with them now read as other stats. To fix: put the mod's table back as "
			               "it was (then they get their old numbers again), and don't play those characters until then. Live Tables' id maps (package 1.2b) will keep numbers in future.";
			g_statsView.entries.push_back(std::move(entry));
		}
		{ // first in the list: the room (Live Tables' count of the limit) or why New does nothing
			Entry             entry;
			const std::string used = UsedText(livetables::Table::ItemStatCost);
			entry.row    = -1;
			entry.title  = used.empty() ? "New needs Live Tables" : "Stats: " + used;
			entry.detail = used.empty() ? "Live Tables is not installed: this tab changes the rows that are there; New does nothing."
			                            : "New adds a row at once (Live Tables). Items save stat numbers in 9 bits: 511 at most (0-510). " + g_handoverNote;
			g_statsView.entries.push_back(std::move(entry));
		}
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* row = bank->stats->rows + i * prop::kStatRow;
			Entry          entry;
			entry.row         = static_cast<int>(i);
			const auto edited = g_statEdits.find(static_cast<uint16_t>(i));
			const bool named  = GameNamed(*bank, static_cast<uint16_t>(i));
			entry.word        = StatWord(*bank, static_cast<uint16_t>(i));
			entry.title       = std::to_string(i) + "  " + (entry.word) + (StatOff(static_cast<uint16_t>(i)) ? "   (disabled)" : "") + (WrittenMade(*bank, static_cast<uint16_t>(i)) ? "   (made by you)" : edited == g_statEdits.end() ? "" : named ? "   (changed)" : "   (made by you)");
			if (prop::IsBlank(row)) {
				entry.detail = "an empty row: no stat uses it (the New button makes a stat of one)";
			} else {
				int places = 0, dependents = 0;
				while (places < prop::kOpListMax && row[prop::kOpList + places * 6 + 4] != 0) {
					++places;
				}
				for (; dependents < prop::kDependentMax; ++dependents) {
					uint16_t other;
					memcpy(&other, row + prop::kDependents + dependents * 2, 2);
					if (other >= count) {
						break;
					}
				}
				uint32_t flags;
				memcpy(&flags, row + prop::kFlags, 4);
				entry.detail = row[prop::kValShift] != 0 ? "kept x" + std::to_string(1 << row[prop::kValShift]) + " (the number shown is 1/" + std::to_string(1 << row[prop::kValShift]) + " of it)" : "kept as shown";
				entry.detail += row[prop::kSaveBits] != 0 ? "; on items: " + std::to_string(row[prop::kSaveBits]) + " bits" + ((flags & prop::kFlagSigned) != 0 ? ", may be negative" : "") : "; not saved on items";
				entry.detail += "; room for " + std::to_string((prop::kOpListMax - places) / 3) + " live properties adding to it";
				if (dependents != 0) {
					entry.detail += "; " + std::to_string(dependents) + " stats are worked out from it";
				}
				if (row[prop::kOpAt] != 0) {
					uint16_t base, target;
					memcpy(&base, row + prop::kOpBase, 2);
					memcpy(&target, row + prop::kOpStat, 2);
					entry.detail += "; itself op " + std::to_string(row[prop::kOpAt]) + (base < count ? " of " + StatWord(*bank, base) : "") + (target < count ? " to " + StatWord(*bank, target) : "");
				}
			}
			g_statsView.entries.push_back(std::move(entry));
		}
		if (LtV4()) { // made properties whose number moved (their codes still resolve; no save involved)
			livetables::MovedRow moved[16];
			const int            n = g_lt.load()->moved(livetables::Table::Properties, 0, moved, 16);
			std::string          said;
			for (int i = 0; i < n && i < 16; ++i) {
				said += (said.empty() ? "" : ", ") + std::string(moved[i].name) + " " + std::to_string(moved[i].was) + " -> " + std::to_string(moved[i].now) + " (data version " + std::to_string(moved[i].dataVersion) + ")";
			}
			if (n > 0) {
				Entry entry;
				entry.row    = -1;
				entry.title  = "Note: made properties have new numbers (" + Cut(said, 70) + ")";
				entry.detail = "A row added or removed above them in the mod's properties.txt moved them: " + said + ". Recipes, affixes and items name properties by their code, so they still find them; nothing to fix.";
				g_propsView.entries.push_back(std::move(entry));
			}
		}
		{
			Entry             entry;
			const std::string used = UsedText(livetables::Table::Properties);
			entry.row    = -1;
			entry.title  = used.empty() ? "New property needs Live Tables" : "Properties: " + used;
			entry.detail = used.empty() ? "Live Tables is not installed: this tab changes the rows that are there; New property does nothing."
			                            : "New property adds a row at once (Live Tables); then fill its lines on its card.";
			g_propsView.entries.push_back(std::move(entry));
		}
		for (size_t i = 0; i < bank->props->count && bank->props->rows == bank->propRows; ++i) {
			const uint8_t* row = bank->propRows + i * prop::kPropRow;
			Entry          entry;
			entry.row = static_cast<int>(i);
			{
				const std::string key   = PropKey(*bank, i);
				const bool        off   = g_off.count(key) != 0;
				const Cells       cells = off ? PropLogical(*bank, i) : RowCells(*bank, row);
				entry.title             = std::to_string(i) + "  " + key + (off ? "   (disabled)" : g_edits.count(key) != 0 ? "   (changed)" : "");
				entry.detail            = off ? "disabled - " : "";
				for (int line = 0; line < 7; ++line) {
					const std::string& func = cells[static_cast<size_t>(line * 4)];
					if (func.empty()) {
						continue;
					}
					entry.detail += (entry.detail.empty() || entry.detail == "disabled - " ? "" : ";  ") + ("func " + func) + (cells[static_cast<size_t>(line * 4 + 1)].empty() ? "" : " " + cells[static_cast<size_t>(line * 4 + 1)])
					              + (cells[static_cast<size_t>(line * 4 + 2)].empty() ? "" : " set " + cells[static_cast<size_t>(line * 4 + 2)])
					              + (cells[static_cast<size_t>(line * 4 + 3)].empty() ? "" : " val " + cells[static_cast<size_t>(line * 4 + 3)]);
				}
				if (MadeByCode(key) != nullptr) {
					entry.title += "   (made by you)";
				}
				if (entry.detail.empty() || entry.detail == "disabled - ") {
					entry.detail += "does nothing (no func)";
				}
			}
			g_propsView.entries.push_back(std::move(entry));
		}
	}
	if (bank != nullptr && bank->affixOn) {
		for (size_t i = 0; i < bank->affixes->count; ++i) {
			int    kind  = 0;
			size_t index = 0;
			AffixPlace(*bank, i, kind, index);
			const affix::Cells cells = AffixCells(*bank, i);
			const uint8_t*     row   = bank->affixes->rows + i * affix::kRow;
			const auto         edit  = g_affixEdits.find(AffixKey(kind, index));
			Entry              entry;
			entry.row   = static_cast<int>(i);
			entry.word  = std::string(kind == kPrefixes ? "pre=" : kind == kSuffixes ? "suf=" : "") + std::to_string(index);
			entry.title = AffixKey(kind, index) + "  " + (cells[affix::kNameCell].empty() ? "(no name)" : cells[affix::kNameCell]) + (g_affixOff.count(AffixKey(kind, index)) != 0 ? "   (disabled)" : "")
			            + (edit == g_affixEdits.end() ? "" : edit->second.was.empty() ? "   (made by you)" : "   (changed)");
			if (affix::IsBlank(row, bank->names)) {
				entry.detail = "an empty row: no affix uses it (New makes an affix of one)";
			} else {
				std::string on, mods;
				for (int t = 0; t < 7; ++t) {
					on += cells[static_cast<size_t>(affix::kTypeCell + t)].empty() ? "" : (on.empty() ? "" : " ") + cells[static_cast<size_t>(affix::kTypeCell + t)];
				}
				for (int m = 0; m < 3; ++m) {
					const std::string* c = &cells[static_cast<size_t>(affix::kModCell + m * 4)];
					if (!c[0].empty()) {
						mods += (mods.empty() ? "" : ", ") + c[0] + (c[1].empty() ? "" : "(" + c[1] + ")") + " " + (c[2].empty() ? "0" : c[2]) + "-" + (c[3].empty() ? "0" : c[3]);
					}
				}
				entry.detail = "level " + (cells[affix::kLevelCell].empty() ? std::string("0") : cells[affix::kLevelCell]) + ", frequency " + (cells[affix::kFrequencyCell].empty() ? std::string("0") : cells[affix::kFrequencyCell])
				             + (cells[affix::kSpawnCell] == "1" ? "" : ", not spawnable") + (on.empty() ? "" : "; on " + on) + (mods.empty() ? "" : "; " + mods)
				             + (kind == kAutomagic ? "" : "; recipe: " + entry.word);
			}
			g_affixView.entries.push_back(std::move(entry));
		}
	}
	BuildLegend(bank);
	for (ListView* view : { &g_statsView, &g_propsView, &g_funcsView, &g_affixView }) {
		Filter(*view);
		const bool listed = std::any_of(view->entries.begin(), view->entries.end(), [&](const Entry& entry) { return entry.row == view->card; });
		if (view->card >= 0 && !listed) {
			view->card = view->cell = -1;
		}
	}
}

// An affix's cell changed (AffixEdit) or the row back to the game's own (AffixRestore), in every
// data version that has the same row. slot = the row in the main data version. Game thread, under the lock.
void RunAffix(const Command& command) {
	std::string& note = g_affixView.note;
	Bank*        main = MainBank();
	if (main == nullptr || !main->affixOn || command.slot < 0 || static_cast<uint64_t>(command.slot) >= main->affixes->count) {
		note = "that affix row is not there";
		return;
	}
	const size_t row  = static_cast<size_t>(command.slot);
	int          kind = 0;
	size_t       index = 0;
	AffixPlace(*main, row, kind, index);
	const std::string key = AffixKey(kind, index);
	const uint8_t*    orig = AffixOrig(*main, row);
	const std::string was  = orig != nullptr ? RowName(orig) : "";
	if (command.kind == Command::AffixOff || command.kind == Command::AffixOn) {
		const bool off  = command.kind == Command::AffixOff;
		int        done = 0;
		for (Bank& bank : g_banks) {
			size_t         at  = 0;
			const uint8_t* own = bank.on && AffixRowOf(bank, kind, index, at) ? AffixOrig(bank, at) : nullptr;
			if (own == nullptr || RowName(own) != was) {
				continue;
			}
			uint8_t* live = bank.affixes->rows + at * affix::kRow;
			if (off) {
				affix::Off(live);
			} else { // back to the edited cells, or the game's own
				const auto edit = g_affixEdits.find(key);
				uint8_t    from[affix::kRow];
				memcpy(from, own, affix::kRow);
				std::string problem;
				if (edit != g_affixEdits.end()) {
					memcpy(from, live, affix::kRow);
					if (!affix::Build(edit->second.cells, bank.names, from, problem)) {
						memcpy(from, own, affix::kRow);
					}
				}
				affix::On(live, from);
			}
			++done;
		}
		if (off) {
			g_affixOff.insert(key);
		} else {
			g_affixOff.erase(key);
		}
		WriteAffixOff();
		note = off ? key + " is disabled: it rolls on no drop and a recipe cannot force it. Items that have it keep it. Enable gives it back."
		           : key + " is enabled again (spawnable and frequency as before).";
		LogF("affix %s: %s in %d data versions", key.c_str(), off ? "disabled" : "enabled", done);
		return;
	}
	if (g_affixOff.count(key) != 0) {
		note = key + " is disabled: Enable it first, then change it.";
		return;
	}
	if (command.kind == Command::AffixRestore && was.empty() && g_affixEdits.count(key) != 0) { // Remove of a made affix: no saved item may carry its id
		std::string why;
		if (!AffixIdKnown(*main, kind, index, why)) {
			note = "Not removed: " + why;
			return;
		}
		if (const std::string held = AffixHolders(kind, index); !held.empty()) {
			note = "Not removed: " + held;
			return;
		}
	}
	if (command.kind == Command::AffixRestore) {
		for (Bank& bank : g_banks) {
			size_t         at = 0;
			const uint8_t* own = AffixRowOf(bank, kind, index, at) ? AffixOrig(bank, at) : nullptr;
			if (bank.on && own != nullptr && RowName(own) == was) {
				memcpy(bank.affixes->rows + at * affix::kRow, own, affix::kRow);
			}
		}
		FreeName(key);
		g_affixEdits.erase(key);
		WriteAffixEdits();
		note = key + " is the game's own row again" + (was.empty() ? " (empty)." : ".");
		LogF("affix %s: back to the game's own row", key.c_str());
		return;
	}
	if (command.kind == Command::AffixEdit && was.empty() && g_affixEdits.count(key) == 0) { // the first cell of a new affix
		std::string why;
		if (!AffixIdKnown(*main, kind, index, why)) {
			note = "Not made: " + why;
			return;
		}
	}
	affix::Cells cells = AffixCells(*main, row);
	if (command.percent < 0 || command.percent >= affix::kCells) {
		return;
	}
	cells[static_cast<size_t>(command.percent)] = command.a == "-" ? "" : command.a;
	std::string problem;
	if (!PutAffix(*main, row, cells, problem)) {
		note = "Not changed: " + problem;
		return;
	}
	cells = AffixCells(*main, row); // as the row now reads
	for (Bank& bank : g_banks) {
		size_t         at = 0;
		const uint8_t* own = &bank != main && bank.on && AffixRowOf(bank, kind, index, at) ? AffixOrig(bank, at) : nullptr;
		if (own != nullptr && RowName(own) == was && !PutAffix(bank, at, cells, problem)) {
			LogF("data version %d, affix %s: %s", bank.version, key.c_str(), problem.c_str());
		}
	}
	if (orig != nullptr && memcmp(main->affixes->rows + row * affix::kRow, orig, affix::kRow) == 0) {
		g_affixEdits.erase(key);
	} else {
		AffixEdit& edit = g_affixEdits[key];
		edit.kind = kind, edit.index = index, edit.was = was, edit.cells = cells;
	}
	WriteAffixEdits();
	note = key + ": " + affix::kFields[command.percent].name + " is now " + (cells[static_cast<size_t>(command.percent)].empty() ? "empty" : "'" + cells[static_cast<size_t>(command.percent)] + "'")
	     + ". It counts for items made from now on.";
	LogF("affix %s: %s", key.c_str(), note.c_str());
}

// ---- stat Disable / Enable / Remove / Rename (Live Tables): asked for under the lock, started by StartCall
struct StatCall {
	enum Kind { None, Disable, Enable, Remove, Rename, SetCells } kind = None;
	int              number = -1;
	std::string      name;
	std::vector<int> props;   // Disable: the property rows to disable with it (Disable all)
	std::string      newName; // Rename
	livetables::Table table = livetables::Table::ItemStatCost;
	std::string      cells;   // SetCells: tab-separated column=value
	size_t           count = 0;
};
StatCall             g_call;      // under the lock: waiting for StartCall
std::deque<StatCall> g_callQueue; // under the lock: more of them, in order (Save on a made row's card)
StatCall          g_callRunning;     // the one Live Tables is working on (one at a time)
std::atomic<bool> g_callBusy { false };

constexpr size_t kCodeMost = 31;
bool CodeLetter(char c) {
	return isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '%' || c == '/' || c == '_' || c == '+' || c == '.';
}

// A changed cell (Edit) or a row back to the game's own (Restore), in every data version that
// has the property. Game thread, under the lock.
void RunEdit(const Command& command) {
	std::string& note = g_propsView.note;
	Bank*        main = MainBank();
	if (main == nullptr || static_cast<uint64_t>(command.slot) >= main->propBase) {
		note = "that row cannot be changed here";
		return;
	}
	const size_t      row = static_cast<size_t>(command.slot);
	const std::string key = PropKey(*main, row);
	if (command.kind == Command::PropOff || command.kind == Command::PropOn) {
		const bool off  = command.kind == Command::PropOff;
		int        done = 0;
		for (Bank& bank : g_banks) {
			size_t      at = 0;
			std::string problem;
			if (!bank.on || bank.props->rows != bank.propRows || !FindRow(bank, key, at)) {
				continue;
			}
			if (off) {
				FuncsOff(bank, at);
			} else if (g_edits.count(key) != 0 ? !PutCells(bank, at, g_edits[key], problem) : (at + 1) * prop::kPropRow > bank.propOrig.size()) {
				LogF("data version %d, property %s: not enabled again: %s", bank.version, key.c_str(), problem.empty() ? "the game's own row is not known" : problem.c_str());
				continue;
			} else if (g_edits.count(key) == 0) {
				memcpy(bank.propRows + at * prop::kPropRow + kPropFunc, bank.propOrig.data() + at * prop::kPropRow + kPropFunc, 7);
			}
			++done;
		}
		if (off) {
			g_off.insert(key);
		} else {
			g_off.erase(key);
		}
		WriteOff();
		note = off ? "'" + key + "' is disabled: it gives nothing to items made from now on. Items that exist keep their stats; everything that names it still loads."
		           : "'" + key + "' is enabled again: items made from now on get it as before.";
		LogF("property %s (row %zu): %s in %d data versions", key.c_str(), row, off ? "disabled" : "enabled", done);
		return;
	}
	if (g_off.count(key) != 0) {
		note = "'" + key + "' is disabled: Enable it first, then change it.";
		return;
	}
	if (command.kind == Command::Restore) {
		for (Bank& bank : g_banks) {
			size_t at = 0;
			if (bank.on && bank.props->rows == bank.propRows && FindRow(bank, key, at) && (at + 1) * prop::kPropRow <= bank.propOrig.size()) {
				memcpy(bank.propRows + at * prop::kPropRow + 2, bank.propOrig.data() + at * prop::kPropRow + 2, prop::kPropRow - 2);
			}
		}
		g_edits.erase(key);
		WriteEdits();
		note = "'" + key + "' is the game's own row again.";
		if (MadeByCode(key) != nullptr) {
			note = "'" + key + "' is back as it was grown (no lines: it gives nothing). Its row stays: Live Tables keeps it.";
		}
		LogF("property %s (row %zu): back to the game's own row", key.c_str(), row);
		return;
	}
	if (command.percent == kCodeCell) { // a new code: Live Tables renames a made property; the game's own are refused
		const std::string to = command.a == "-" ? "" : command.a;
		size_t            other = 0;
		bool              plain = !to.empty() && to.size() <= kCodeMost && to.rfind("live_", 0) != 0;
		for (const char c : to) {
			plain = plain && CodeLetter(c);
		}
		if (MadeByCode(key) == nullptr) {
			note = "Not renamed: '" + key + "' is the game's (or the mod's) own property. Cube recipes (cubemain mods), uniques (prop1-12), set items and sets, runewords, gems and the magic "
			       "affixes (mod1-3) name it by this code when the tables are read, so they would not find it from the next start.";
		} else if (!plain) {
			note = "Not renamed: a code is letters, digits and - % / _ + . (31 at most, not starting with live_).";
		} else if (FindRow(*main, to, other)) {
			note = "Not renamed: property " + std::to_string(other) + " has the code '" + to + "' already.";
		} else if (to != key) {
			g_call         = {};
			g_call.kind    = StatCall::Rename;
			g_call.number  = static_cast<int>(row);
			g_call.name    = key;
			g_call.newName = to;
			g_call.table   = livetables::Table::Properties;
			note           = "Renaming '" + key + "' to '" + to + "' (Live Tables) ...";
		}
		return;
	}
	Cells cells = RowCells(*main, main->propRows + row * prop::kPropRow);
	if (command.percent < 0 || command.percent >= kCells) {
		return;
	}
	cells[static_cast<size_t>(command.percent)] = command.a == "-" ? "" : command.a;
	std::string problem;
	if (!PutCells(*main, row, cells, problem)) {
		note = "Not changed: " + problem;
		return;
	}
	cells = RowCells(*main, main->propRows + row * prop::kPropRow); // as the row now reads
	for (Bank& bank : g_banks) {
		size_t at = 0;
		if (bank.on && &bank != main && FindRow(bank, key, at) && !PutCells(bank, at, cells, problem)) {
			LogF("data version %d, property %s: %s", bank.version, key.c_str(), problem.c_str());
		}
	}
	if ((row + 1) * prop::kPropRow <= main->propOrig.size() && memcmp(main->propRows + row * prop::kPropRow, main->propOrig.data() + row * prop::kPropRow, prop::kPropRow) == 0) {
		g_edits.erase(key);
	} else {
		g_edits[key] = cells;
	}
	WriteEdits();
	static const char* const columns[] = { "func", "stat", "set", "val" };
	note = "'" + key + "' line " + std::to_string(command.percent / 4 + 1) + " " + columns[command.percent % 4] + " is now " + (command.a.empty() || command.a == "-" ? "empty" : "'" + cells[static_cast<size_t>(command.percent)] + "'")
	     + ". It counts for items made from now on.";
	LogF("property %s (row %zu): %s", key.c_str(), row, note.c_str());
}


void AskRemove(uint16_t stat);
bool StatOff(uint16_t stat);
std::string GameView(const Bank& bank, uint16_t stat);
void SetAdvDisplay(Bank& main, uint16_t stat, int value);

// A stat's cell changed (StatEdit), a stat back to the game's own row - for a made stat that
// removes it - (StatRestore). Game thread, under the lock.
void RunStat(const Command& command) {
	std::string& note = g_statsView.note;
	Bank*        main = MainBank();
	if (main == nullptr) {
		note = "the game's tables are not there";
		return;
	}
	uint16_t  stat = static_cast<uint16_t>(command.slot);
	StatCells cells;
	if (command.kind == Command::StatRemoveAsk) {
		AskRemove(stat);
		return;
	}
	if (command.kind == Command::StatGameView) {
		g_statsView.gameText = GameView(*main, stat);
		return;
	}
	if (command.kind == Command::StatAdv) {
		SetAdvDisplay(*main, stat, command.percent);
		return;
	}
	if (command.kind == Command::StatLine) { // a made stat handed to Live Tables: its line onto its new row
		for (Bank& bank : g_banks) {
			const auto  line = g_statEdits.find(stat);
			std::string problem;
			if (bank.on && line != g_statEdits.end() && stat < bank.stats->count && GameStatName(bank, stat) == line->second[kNameCell]
				&& !ApplyStat(bank, stat, Strings(bank, stat, line->second), D2RL::InvalidPlayerHandle, false, problem, nullptr)) {
				LogF("data version %d, stat %d: %s", bank.version, stat, problem.c_str());
			}
		}
		return;
	}
	if (stat >= main->stats->count || (stat + 1u) * prop::kStatRow > main->statOrig.size()) {
		note = "there is no such stat";
		return;
	} else if (command.kind == Command::StatRestore) {
		cells = StatRowCells(*main, main->statOrig.data() + stat * prop::kStatRow);
	} else {
		if (command.percent < 0 || command.percent >= kStatCells) {
			return;
		}
		cells = StatRowCells(*main, main->stats->rows + stat * prop::kStatRow);
		if (const auto edited = g_statEdits.find(stat); edited != g_statEdits.end()) {
			cells[kNameCell] = edited->second[kNameCell];
		}
		if (command.percent == kNameCell && GameNamed(*main, stat)) {
			note = "Not renamed: '" + GameStatName(*main, stat) + "' is the game's (or the mod's) own stat. Its name is how the tables read at load find it - other stats' op base / op stat and "
			       "maxstat in itemstatcost.txt, the stat1-7 cells of properties.txt, and any mod file that names it - so a new name would leave them pointing at nothing from the next start.";
			return;
		}
		if (command.percent == kNameCell && WrittenMade(*main, stat)) { // a made stat: Live Tables renames it in the game's name table
			const std::string to = command.a == "-" ? "" : command.a;
			uint16_t          other = 0;
			if (to.empty() || to.size() > 40 || (to[0] >= '0' && to[0] <= '9') || to.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
				note = "Not renamed: a name is letters, digits, _ and - only, not starting with a digit, 40 at most.";
			} else if (StatNumber(*main, to, other)) {
				note = "Not renamed: stat " + std::to_string(other) + " is called '" + to + "' already.";
			} else {
				g_call         = {};
				g_call.kind    = StatCall::Rename;
				g_call.number  = stat;
				g_call.name    = GameStatName(*main, stat);
				g_call.newName = to;
				g_call.table   = livetables::Table::ItemStatCost;
				note           = "Renaming '" + g_call.name + "' to '" + to + "' (Live Tables) ...";
			}
			return;
		}
		cells[static_cast<size_t>(command.percent)] = command.a == "-" ? "" : command.a;
	}
	const std::string was     = GameStatName(*main, stat);
	const bool        written = WrittenMade(*main, stat); // before the line may go
	std::string       problem;
	int               unchecked = 0; // bit 0: no character in a game; then 2 = saves scanned, 4 = Save Scan missing
	if (!ApplyStat(*main, stat, cells, command.player, true, problem, &unchecked)) {
		note = (command.kind == Command::StatRestore ? "Not done: " : "Not changed: ") + problem;
		return;
	}
	for (Bank& bank : g_banks) {
		if (bank.on && &bank != main && stat < bank.stats->count && GameStatName(bank, stat) == was && !ApplyStat(bank, stat, cells, command.player, false, problem, nullptr)) {
			LogF("data version %d, stat %d: %s", bank.version, stat, problem.c_str());
		}
	}
	const std::string name = cells[kNameCell];
	cells                  = StatRowCells(*main, main->stats->rows + stat * prop::kStatRow); // as the row now reads
	cells[kNameCell]       = name;
	cells[kWasCell]        = was;
	const bool same        = memcmp(main->stats->rows + stat * prop::kStatRow, main->statOrig.data() + stat * prop::kStatRow, prop::kStatRow) == 0;
	if (command.kind == Command::StatRestore && written) { // a made stat back to its row as grown: it stays a made stat
		cells[kNameCell]  = GameStatName(*main, stat);
		cells[kWasCell]   = cells[kNameCell];
		g_statEdits[stat] = cells;
	} else if (command.kind == Command::StatRestore || (same && name.empty())) {
		g_statEdits.erase(stat);
	} else {
		g_statEdits[stat] = cells;
	}
	WriteStatEdits();
	const std::string who = "Stat " + std::to_string(stat) + " (" + StatWord(*main, stat) + ")";
	if (command.kind == Command::StatRestore) {
		note = written ? "Stat " + std::to_string(stat) + " is back as it was grown (13 bits, no text, no op). Its row stays: Live Tables keeps it." : who + " is the game's own row again.";
	} else {
		note = who + ": " + (command.percent == kNameCell ? "name" : kFields[command.percent].name) + " is now " + (command.a.empty() || command.a == "-" ? "empty" : "'" + cells[static_cast<size_t>(command.percent)] + "'") + ".";
	}
	if ((unchecked & 2) != 0) {
		note += " Every saved character and the shared stash were read first: no item holds it.";
	} else if ((unchecked & 4) != 0) {
		note += (unchecked & 1) != 0 ? " No character is in a game, so no item could be checked." : "";
		note += " The Save Scan plugin is not installed: saved characters and the shared stash were NOT checked.";
	}
	LogF("%s", note.c_str());
}

void Ask();

void Redraw() {
	if (g_book == nullptr) {
		return;
	}
	g_book->redraw(g_statsView.handle);
	g_book->redraw(g_propsView.handle);
	g_book->redraw(g_funcsView.handle);
	g_book->redraw(g_affixView.handle);
	if (g_legendDirty.exchange(false) && modbook::HasLegend(g_book)) {
		for (const int handle : g_legendHandle) {
			if (handle != 0) {
				g_book->legendChanged(handle);
			}
		}
	}
}

// A tab's own buttons, on the Mod Book's action row: slots 232 wide at x 82 / 322 / 562 / 802,
// filled in the order of the calls. A Mod Book before 0.8.0 has no action row: the same slots are
// drawn here, at the old button line (y 172, moved down with the tab rows).
struct Actions {
	modbook::Page* page;
	int            used = 0;   // slots taken (the fallback's count)
	int            below = 268; // the first y under the rows so far
	void add(const char* click, const std::string& text, const char* color, int slots = 1) {
		slots = slots < 1 ? 1 : slots > modbook::kActionsInRow ? modbook::kActionsInRow : slots;
		if (modbook::HasRows(g_book)) {
			below = g_book->action(page, click, text.c_str(), color, slots);
			return;
		}
		if (used % modbook::kActionsInRow + slots > modbook::kActionsInRow) {
			used += modbook::kActionsInRow - used % modbook::kActionsInRow;
		}
		static const int widths[] = { 232, 470, 700, 742 };
		const int        row = used / modbook::kActionsInRow;
		g_book->button(page, click, 82 + (used % modbook::kActionsInRow) * (modbook::kActionSlot + modbook::kActionGap), 172 + row * modbook::kActionRowStep, widths[slots - 1], text.c_str(),
			color != nullptr ? color : "$FontColorLightGold");
		used += slots;
		below = 268 + row * modbook::kActionRowStep;
	}
	int top() const { return below + 6; } // where the page's own things start
};

void Search(modbook::Page* page, const ListView& view, const char* hint) {
	const std::string typed = view.words.empty() ? std::string() : Cut(view.words, 34) + "_";
	if (modbook::HasRows(g_book)) {
		g_book->search(page, typed.c_str(), hint);
		return;
	}
	g_book->image(page, "SearchSlot", 210, 138, "PANEL\\Modals\\InputSlot");
	g_book->label(page, "Search", 250, 166, 660, 56, typed.empty() ? "$FontColorGoldYellow" : "$FontColorWhite", "left", 0, typed.empty() ? hint : typed.c_str());
}

void DrawList(modbook::Page* page, const ListView& view, const char* hint, const char* tip, const char* newButton) {
	Search(page, view, hint);
	int top = 268;
	if (newButton != nullptr) {
		Actions row { page };
		row.add("New", newButton, "$FontColorGreen");
		top = row.top();
	}
	for (size_t i = 0; i < kListRows; ++i) {
		const size_t      at    = view.top + i;
		const bool        there = at < view.found.size();
		const int         y     = top + static_cast<int>(i) * 106;
		const std::string n     = std::to_string(i);
		const Entry*      entry = there ? &view.entries[view.found[at]] : nullptr;
		g_book->label(page, ("Title" + n).c_str(), 82, y, 990, 36, "$FontColorLightGold", "left", 0, there ? Cut(entry->title, 58).c_str() : "");
		g_book->label(page, ("Detail" + n).c_str(), 120, y + 36, 950, 66, "$FontColorGoldYellow", "left", 1, there ? Cut(entry->detail, 108).c_str() : "");
		if (there) {
			g_book->spot(page, ("Hover" + n).c_str(), 82, y, 980, 102, ("Row" + n).c_str(), (entry->title + "\n" + entry->detail + "\n" + tip).c_str());
		}
	}
	char text[120];
	if (view.found.empty()) {
		snprintf(text, sizeof(text), view.entries.empty() ? "nothing read yet (the game has not read its tables)" : "nothing has all of these words");
	} else {
		snprintf(text, sizeof(text), "%zu - %zu of %zu", view.top + 1, view.top + kListRows < view.found.size() ? view.top + kListRows : view.found.size(), view.found.size());
	}
	g_book->bar(page, top, view.top, kListRows, view.found.size());
	g_book->pager(page, view.top > 0 ? 1 : 0, view.top + kListRows < view.found.size() ? 1 : 0, text);
}

// What names a property, for the question before it is disabled: the affixes in memory (exact),
// and the mod's tables that list properties, read from the files the game read them from (Exalted
// Items' copy first, then the mod's). A table in neither is the game's own; it is named, not read.
std::string PropUses(const Bank& bank, size_t row, const std::string& code) {
	auto lower = [](std::string text) {
		for (char& c : text) {
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		}
		return text;
	};
	const std::string        want = lower(code);
	std::vector<std::string> parts;
	if (bank.affixOn) {
		std::vector<std::string> names;
		for (size_t i = 0; i < bank.affixes->count; ++i) {
			const uint8_t* at = bank.affixes->rows + i * affix::kRow;
			bool           uses = false;
			for (int m = 0; m < 3; ++m) {
				uint32_t mod;
				memcpy(&mod, at + 0x24 + m * 0x10, 4);
				uses = uses || mod == row;
			}
			if (uses && !affix::IsBlank(at, bank.names)) {
				int    kind  = 0;
				size_t index = 0;
				AffixPlace(bank, i, kind, index);
				const std::string name = AffixCells(bank, i)[affix::kNameCell];
				names.push_back((name.empty() ? std::string("(no name)") : name) + " " + AffixKey(kind, index));
			}
		}
		if (!names.empty()) {
			std::string list;
			for (size_t i = 0; i < names.size() && i < 4; ++i) {
				list += (i == 0 ? "" : ", ") + names[i];
			}
			parts.push_back(std::to_string(names.size()) + (names.size() == 1 ? " affix (" : " affixes (") + list + (names.size() > 4 ? ", ..." : "") + ")");
		}
	}
	static const char* const files[] = { "cubemain.txt", "uniqueitems.txt", "setitems.txt", "sets.txt", "runes.txt", "gems.txt" };
	std::string              unread;
	const std::wstring       game = g_loaderDir.substr(0, g_loaderDir.find_last_of(L"\\/"));
	for (const char* file : files) {
		const std::wstring mod  = Widen(g_modName);
		const std::wstring tail = L"\\data\\global\\excel\\" + Widen(file);
		std::string        text;
		const bool         copy = ReadWhole(g_loaderDir + L"\\exalted-items\\" + mod + tail, text); // the redirected copy the game reads, when there is one
		if (!copy && !ReadWhole(game + L"\\mods\\" + mod + L"\\" + mod + L".mpq" + tail, text)) {
			unread += (unread.empty() ? "" : ", ") + std::string(file);
			continue;
		}
		std::vector<std::vector<std::string>> table;
		for (size_t at = 0; at < text.size();) {
			size_t end = text.find('\n', at);
			end        = end == std::string::npos ? text.size() : end;
			std::string line = text.substr(at, end - at);
			at               = end + 1;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			std::vector<std::string> cells;
			for (size_t from = 0; from <= line.size();) {
				size_t tab = line.find('\t', from);
				tab        = tab == std::string::npos ? line.size() : tab;
				cells.push_back(line.substr(from, tab - from));
				from = tab + 1;
			}
			table.push_back(std::move(cells));
		}
		if (table.empty()) {
			continue;
		}
		std::vector<size_t> columns; // the columns that hold property codes
		size_t              label = 0;
		bool                named = false;
		for (size_t c = 0; c < table[0].size(); ++c) {
			const std::string h = lower(table[0][c]);
			const bool        modN = (h.rfind("mod ", 0) == 0 || h.rfind("b mod ", 0) == 0 || h.rfind("c mod ", 0) == 0) && !h.empty() && isdigit(static_cast<unsigned char>(h.back())) != 0;
			const bool        listed = (h.rfind("prop", 0) == 0 && h.size() > 4 && isdigit(static_cast<unsigned char>(h[4])) != 0) || h.rfind("aprop", 0) == 0 || h.rfind("pcode", 0) == 0 || h.rfind("fcode", 0) == 0
			                    || h.rfind("t1code", 0) == 0 || (h.find("mod") != std::string::npos && h.size() > 4 && h.compare(h.size() - 4, 4, "code") == 0);
			if (modN || listed) {
				columns.push_back(c);
			}
			if (!named && (h == "description" || h == "index" || h == "name" || h == "*rune name")) {
				label = c;
				named = true;
			}
		}
		std::vector<std::string> rows;
		for (size_t r = 1; r < table.size(); ++r) {
			for (const size_t c : columns) {
				if (c < table[r].size() && lower(table[r][c]) == want) {
					rows.push_back(label < table[r].size() && !table[r][label].empty() ? table[r][label] : "row " + std::to_string(r));
					break;
				}
			}
		}
		if (!rows.empty()) {
			std::string list;
			for (size_t i = 0; i < rows.size() && i < 3; ++i) {
				list += (i == 0 ? "" : ", ") + Cut(rows[i], 30);
			}
			parts.push_back(std::string(file) + (copy ? " (Exalted Items' copy)" : " (the mod's)") + ": " + std::to_string(rows.size()) + " (" + list + (rows.size() > 3 ? ", ..." : "") + ")");
		}
	}
	std::string said = parts.empty() ? "Nothing in the affixes or the mod's tables names it." : "Named by: ";
	for (size_t i = 0; i < parts.size(); ++i) {
		said += (i == 0 ? "" : "; ") + parts[i];
	}
	if (!unread.empty()) {
		said += ". Not read (the game's own files, not in the mod): " + unread;
	}
	return said + ".";
}

// A card's first action row, the same on every card: Back, Save (N), Discard, Restore. They always
// stand there (so nothing moves when a cell is typed); Save / Discard / Restore are gold while there
// is nothing for them to do. restore: the 4th button's words ("Restore" / "Remove"); edited: it has something to restore.
Actions CardActions(modbook::Page* page, const ListView& v, const char* restore, bool edited) {
	const size_t count = v.pending.size() + (v.cell >= 0 && v.pending.count(v.cell) == 0 ? 1 : 0);
	Actions      row { page };
	row.add("Back", "Back", "$FontColorWhite");
	row.add("Save", count != 0 ? "Save (" + std::to_string(count) + ")" : std::string("Save"), count != 0 ? "$FontColorGreen" : nullptr);
	row.add("Discard", "Discard", count != 0 ? "$FontColorLightRed" : nullptr);
	row.add("Restore", restore, edited ? "$FontColorLightRed" : nullptr);
	return row;
}

// One property row: its 7 lines of func / stat / set / val, each cell a place to type in.
void DrawCard(modbook::Page* page) {
	const ListView& v    = g_propsView;
	const Bank*     bank = MainBank();
	const bool      there = bank != nullptr && bank->props->rows == bank->propRows && static_cast<uint64_t>(v.card) < bank->props->count;
	const std::string key = there ? PropKey(*bank, static_cast<size_t>(v.card)) : std::string();
	const bool      off   = there && g_off.count(key) != 0;
	if (there && v.asking) { // the question before Disable / Remove
		Actions ask { page };
		ask.add("OffYes", v.askRemove ? "Remove it" : "Disable it", "$FontColorLightRed");
		ask.add("OffNo", "Cancel", "$FontColorWhite");
		g_book->title(page, ((v.askRemove ? "Remove " : "Disable ") + key + "?").c_str());
		g_book->label(page, "Ask", 82, ask.top() + 20, 990, 360, "$FontColorLightGold", "left", 1,
			(v.askRemove ? "Remove '" + key + "', a property you made? Live Tables takes its row out of the property table now, in every data version, and its lines leave this plugin's edit "
			                                  "files. Items that exist keep the stats they have (items save stats, not properties). Refused while anything still names the code."
			             : "Disable '" + key + "'? Its 7 lines will do nothing: items made from now on get nothing from it. The row stays, so every recipe, affix and item that names it still loads, "
			                                   "and items that exist keep the stats they have. Enable on its card gives it back.")
				.c_str());
		g_book->label(page, "Uses", 82, ask.top() + 400, 990, 500, "$FontColorGoldYellow", "left", 1, v.uses.c_str());
		return;
	}
	const bool made = there && MadeByCode(key) != nullptr;
	Actions    row  = CardActions(page, v, "Restore", there && g_edits.count(key) != 0);
	if (there && static_cast<size_t>(v.card) < bank->propBase) {
		row.add("Off", off ? "Enable" : "Disable", off ? "$FontColorGreen" : "$FontColorLightRed");
		if (made && LtV2()) { // Live Tables 0.3.0: the last property row it grew
			row.add("PropRemove", "Remove", "$FontColorLightRed");
		}
	}
	const int top = row.top();
	if (!there) {
		g_book->label(page, "Gone", 82, top + 32, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	const size_t      line0 = static_cast<size_t>(v.card);
	const bool        own   = line0 < bank->propBase && !off;
	const Cells       cells = off ? PropLogical(*bank, line0) : RowCells(*bank, bank->propRows + line0 * prop::kPropRow);
	g_book->title(page, ("Property " + std::to_string(line0) + ": " + key + (off ? " (disabled)" : made ? " (made by you)" : "")).c_str());
	static const char* const columns[] = { "func", "stat", "set", "val" };
	static const char* const tips[]    = { "func: what the line does (a number from the Funcs tab or the Legend; empty = the line does nothing)", "stat: the stat the line works on (a name from the Stats tab, or its number)",
		"set: 1 = the value replaces the stat's, 0 = it is added (the game's data guide)", "val: a fixed number some funcs use (func 21: the stat's param)" };
	const int xs[] = { 130, 290, 780, 900 }, widths[] = { 140, 470, 100, 170 };
	for (int c = 0; c < 4; ++c) {
		g_book->label(page, (std::string("Head") + columns[c]).c_str(), xs[c], top, widths[c], 36, "$FontColorGoldYellow", "left", 0, columns[c]);
	}
	for (int line = 0; line < 7; ++line) {
		const int         y = top + 48 + line * 96;
		const std::string n = std::to_string(line);
		g_book->label(page, ("Line" + n).c_str(), 82, y, 40, 36, "$FontColorGoldYellow", "left", 0, std::to_string(line + 1).c_str());
		for (int c = 0; c < 4; ++c) {
			const int         cell   = line * 4 + c;
			const std::string name   = "Cell" + std::to_string(cell);
			const bool        typing = cell == v.cell;
			const auto        typed  = v.pending.find(cell);
			const std::string now    = typed != v.pending.end() ? typed->second + "*" : cells[static_cast<size_t>(cell)];
			const std::string text   = typing ? v.cellText + "_" : now.empty() ? "-" : now;
			g_book->label(page, ("Text" + name).c_str(), xs[c], y, widths[c], 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0, Cut(text, c == 1 ? 28 : 9).c_str());
			if (own) {
				g_book->spot(page, ("Spot" + name).c_str(), xs[c], y, widths[c] - 10, 40, name.c_str(), tips[c]);
			}
		}
		const int func = atoi(cells[static_cast<size_t>(line * 4)].c_str());
		const bool rolled = func == 1 || func == 2 || func == 3 || func == 8; // 0x3d4620: the amount is rolled from the naming row's min..max (0x3d5860); val is not read
		g_book->label(page, ("Words" + n).c_str(), 130, y + 40, 940, 36, "$FontColorGoldYellow", "left", 0,
			func != 0 ? Cut(std::string("func ") + std::to_string(func) + ": " + (rolled ? "the amount is the min..max of the recipe / affix / item that names it; val is not used" : FuncWords(func)), 110).c_str() : "");
	}
	const int below0 = top + 48 + 7 * 96;
	{ // the code: a made property's can be renamed (Live Tables); the game's own say why not on Save
		const bool        typing = v.cell == kCodeCell;
		const auto        typed  = v.pending.find(kCodeCell);
		const std::string text   = typing ? v.cellText + "_" : typed != v.pending.end() ? typed->second + "*" : key;
		g_book->label(page, "HeadCode", 82, below0, 200, 36, "$FontColorGoldYellow", "left", 0, "code");
		g_book->label(page, "TextCellCode", 290, below0, 600, 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0, Cut(text, 31).c_str());
		if (own) {
			g_book->spot(page, "SpotCellCode", 82, below0, 820, 40, ("Cell" + std::to_string(kCodeCell)).c_str(),
				made ? "The code recipes, affixes and items name it by. Type a new one and Save: Live Tables renames it, its number stays, and everything that names it follows."
				     : "The game's (or the mod's) own code: it can't be renamed - the mod's files name it by this code.");
		}
	}
	const int below = below0 + 48;
	g_book->label(page, "Note", 82, below, 990, 90, off && v.note.empty() ? "$FontColorLightRed" : "$FontColorGreen", "left", 1,
		off && v.note.empty() ? "Disabled: these lines do nothing in the game now (it gives nothing to items made from now on). Enable gives them back." : v.note.c_str());
	g_book->label(page, "Help", 82, below + 96, 990, 1346 - (below + 96), "$FontColorGoldYellow", "left", 1,
		own ? "Click a cell, type, Enter keeps it (Tab: keeps it and goes to the next cell, Esc: leaves it); kept cells show green with a *. Save sends them to the game. With a func or stat cell open, a click in the Funcs or Stats tab, "
		      "or Insert in the Legend, puts it in. Saved changes count for items made from then on; items that exist keep the stats they have."
		    : "");
}

// One stat row: 16 cells of it, and the plugin's name for a stat the game has no name for.
void DrawStatCard(modbook::Page* page) {
	const ListView& v      = g_statsView;
	const Bank*     bank   = MainBank();
	const bool      there  = bank != nullptr && v.card >= 0 && static_cast<uint64_t>(v.card) < bank->stats->count;
	const uint16_t  stat   = static_cast<uint16_t>(v.card);
	const auto      edited = g_statEdits.find(stat);
	const bool      named  = there && GameNamed(*bank, stat);
	const bool      off    = there && StatOff(stat);
	if (there && v.asking) { // the question before Disable / Remove
		Actions ask { page };
		if (v.askRemove) {
			ask.add("RemoveYes", "Remove it", "$FontColorLightRed");
		} else {
			ask.add("OffYes", "Disable it", "$FontColorLightRed");
			if (!v.askProps.empty()) {
				ask.add("OffAll", "Disable all", "$FontColorLightRed");
			}
		}
		ask.add("OffNo", "Cancel", "$FontColorWhite");
		const std::string who = std::to_string(stat) + " (" + StatWord(*bank, stat) + ")";
		g_book->title(page, ((v.askRemove ? "Remove stat " : "Disable stat ") + who + "?").c_str());
		g_book->label(page, "Ask", 82, ask.top() + 20, 990, 300, "$FontColorLightGold", "left", 1,
			(v.askRemove ? "Remove stat " + who + "? Its row goes from the stat table. Live Tables refuses while anything still names it (a property line, another stat's op)."
			             : "Disable stat " + who + "? It keeps its number and how it is saved (saves keep loading, items keep the value), but it does nothing: no op, no line on items, no item "
			               "event. Enable gives it back. 'Disable all' also disables the properties that name it.")
				.c_str());
		g_book->label(page, "Uses", 82, ask.top() + 340, 990, 640, "$FontColorGoldYellow", "left", 1, v.uses.c_str());
		return;
	}
	Actions row = CardActions(page, v, "Restore", edited != g_statEdits.end() && !off);
	if (there && LtV2()) {
		row.add("StatOff", off ? "Enable" : "Disable", off ? "$FontColorGreen" : "$FontColorLightRed");
		if (WrittenMade(*bank, stat)) { // made by you: Live Tables removes only rows it grew
			row.add("StatRemove", "Remove", "$FontColorLightRed");
		}
	}
	if (there) {
		static const char* const kAdv[] = { "Adv: never", "Adv: always", "Adv: not 0" };
		const int                adv    = bank->stats->rows[stat * prop::kStatRow + kAdvDisplayAt];
		row.add("GameView", v.gameView ? "Cells" : "Game view", "$FontColorLightTeal");
		row.add("AdvCycle", adv <= 2 ? kAdv[adv] : "Adv: ?", "$FontColorLightTeal");
	}
	const int top = row.top();
	if (!there) {
		g_book->label(page, "Gone", 82, top + 32, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	if (v.gameView) { // How the game sees it
		g_book->title(page, ("Stat " + std::to_string(stat) + ": how the game sees it").c_str());
		g_book->label(page, "GameText", 82, top + 10, 990, 1346 - (top + 10) - 140, "$FontColorLightGold", "left", 1, v.gameText.empty() ? "Reading it on the game thread ..." : v.gameText.c_str());
		g_book->label(page, "Note", 82, 1346 - 130, 990, 120, "$FontColorGreen", "left", 1,
			(v.note.empty() ? "Read from the game's memory as it is now. 'Adv:' sets whether the Advanced Stats panel shows it (never / always / when not 0). 'Cells' goes back."
			                : v.note)
				.c_str());
		return;
	}
	const uint8_t* srow  = bank->stats->rows + stat * prop::kStatRow;
	StatCells      cells = StatRowCells(*bank, srow);
	cells[kNameCell]     = edited != g_statEdits.end() ? edited->second[kNameCell] : "";
	g_book->title(page, ("Stat " + std::to_string(stat) + ": " + StatWord(*bank, stat) + (off ? " (disabled)" : "")).c_str());
	static const char* const groups[] = { "saved", "text", "op" };
	constexpr int            kStep    = 50;
	for (int i = 0; i < kStatCells; ++i) {
		if (i == kNameCell && named) {
			continue;
		}
		const int         y      = top + i * kStep;
		const std::string n      = std::to_string(i);
		const bool        typing = i == v.cell;
		const auto        typed  = v.pending.find(i);
		const std::string now    = typed != v.pending.end() ? typed->second + "*" : cells[static_cast<size_t>(i)];
		const std::string text   = typing ? v.cellText + "_" : now.empty() ? "-" : now;
		std::string       hint   = i == kNameCell ? "your name for it (used in the lists and in stat cells)" : groups[kFields[i].group];
		if (i < kNameCell && kFields[i].kind == kCellString) {
			const std::string words = StringText(static_cast<uint16_t>(FieldValue(srow, kFields[i])));
			hint                    = words.empty() ? hint : "\"" + words + "\"";
		}
		g_book->label(page, ("Name" + n).c_str(), 82, y, 300, 36, "$FontColorGoldYellow", "left", 0, i == kNameCell ? "name" : kFields[i].name);
		g_book->label(page, ("Text" + n).c_str(), 390, y, 330, 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0, Cut(text, 19).c_str());
		g_book->label(page, ("Hint" + n).c_str(), 730, y, 340, 36, "$FontColorGoldYellow", "left", 0, Cut(hint, 20).c_str());
		g_book->spot(page, ("Spot" + n).c_str(), 82, y, 980, kStep - 4, ("Cell" + n).c_str(),
			i == kNameCell ? "The plugin's own name for a stat you made. The game does not know it; this plugin's lists and cells do." : kFields[i].tip);
	}
	g_book->label(page, "Note", 82, top + kStatCells * kStep + 8, 990, 130, "$FontColorGreen", "left", 1,
		v.note.empty() ? "Click a line, type, Enter keeps it (Tab: next line, Esc: leaves it); kept lines show green with a *. Save sends them to the game. Hover a line for what it means; the Legend has the numbers. 'saved' lines are refused while you carry an item with the stat."
		               : v.note.c_str());
}

// One affix row: 35 cells in two columns, the three ways to keep a made affix (off drops or not), and the recipe word.
void DrawAffixCard(modbook::Page* page) {
	const ListView& v     = g_affixView;
	const Bank*     bank  = MainBank();
	const bool      there = bank != nullptr && bank->affixOn && v.card >= 0 && static_cast<uint64_t>(v.card) < bank->affixes->count;
	int             kind  = 0;
	size_t          index = 0;
	if (there) {
		AffixPlace(*bank, static_cast<size_t>(v.card), kind, index);
	}
	const auto edited = there ? g_affixEdits.find(AffixKey(kind, index)) : g_affixEdits.end();
	const bool off    = there && g_affixOff.count(AffixKey(kind, index)) != 0;
	if (there && v.asking) { // the question before Disable
		Actions ask { page };
		ask.add("OffYes", "Disable it", "$FontColorLightRed");
		ask.add("OffNo", "Cancel", "$FontColorWhite");
		g_book->title(page, ("Disable " + AffixKey(kind, index) + "?").c_str());
		g_book->label(page, "Ask", 82, ask.top() + 20, 990, 360, "$FontColorLightGold", "left", 1,
			("Disable " + AffixKey(kind, index) + "? Spawnable and frequency become 0 in the game's memory: it rolls on no drop, and a recipe's pre= / suf= cannot force it. Items that "
			 "have it keep it, and saves keep loading. Its cells are kept; Enable gives them back.")
				.c_str());
		return;
	}
	Actions row = CardActions(page, v, edited != g_affixEdits.end() && edited->second.was.empty() ? "Remove" : "Restore", edited != g_affixEdits.end() && !off);
	row.add("CubeOnly", "Cube only", "$FontColorLightTeal");
	row.add("RareDrops", "Rare drops", "$FontColorLightTeal");
	row.add("NormalDrops", "Normal drops", "$FontColorLightTeal");
	if (there && !affix::IsBlank(bank->affixes->rows + static_cast<size_t>(v.card) * affix::kRow, bank->names)) {
		row.add("AffixOff", off ? "Enable" : "Disable", off ? "$FontColorGreen" : "$FontColorLightRed");
	}
	const int top = row.top();
	if (!there) {
		g_book->label(page, "Gone", 82, top + 32, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	const affix::Cells cells = AffixCells(*bank, static_cast<size_t>(v.card));
	g_book->title(page, (std::string(kind == kPrefixes ? "Prefix " : kind == kSuffixes ? "Suffix " : "Automagic ") + std::to_string(index) + (cells[affix::kNameCell].empty() ? "" : ": " + cells[affix::kNameCell])
	                     + (off ? " (disabled)" : ""))
	                        .c_str());
	constexpr int kPerColumn = 18, kStep = 46;
	for (int i = 0; i < affix::kCells; ++i) {
		const bool         right  = i >= kPerColumn;
		const int          y      = top + (right ? i - kPerColumn : i) * kStep;
		const int          x      = right ? 585 : 82;
		const std::string  n      = std::to_string(i);
		const bool         typing = i == v.cell;
		const auto         typed  = v.pending.find(i);
		const std::string  now    = typed != v.pending.end() ? typed->second + "*" : cells[static_cast<size_t>(i)];
		const std::string  text   = typing ? v.cellText + "_" : now.empty() ? "-" : now;
		g_book->label(page, ("Name" + n).c_str(), x, y, 170, 36, "$FontColorGoldYellow", "left", 0, affix::kFields[i].name);
		g_book->label(page, ("Text" + n).c_str(), x + 175, y, right ? 310 : 300, 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0,
			Cut(text, i == affix::kNameCell ? 17 : 16).c_str());
		g_book->spot(page, ("Spot" + n).c_str(), x, y, 480, kStep - 4, ("Cell" + n).c_str(), affix::kFields[i].tip);
	}
	const std::string word = kind == kPrefixes ? "pre=" + std::to_string(index) : kind == kSuffixes ? "suf=" + std::to_string(index) : "";
	g_book->label(page, "Note", 82, top + kPerColumn * kStep + 6, 990, 120, v.note.empty() ? "$FontColorGoldYellow" : "$FontColorGreen", "left", 1,
		(!v.note.empty() ? v.note
		                 : (word.empty() ? std::string("Automagic rows are the affixes an item base gives itself (items.txt auto prefix).")
		                                 : "In a recipe: " + word + " (with mag), e.g. cm1,mag," + word + ". The game takes it only if spawnable is 1, frequency above 0, its level reached and the item's type listed.")
		                       + " Cube only / Rare drops / Normal drops fill the cells for you; Save sends them.")
			.c_str());
}

// The Affixes tab before a New: which kind.
void DrawAffixChoice(modbook::Page* page) {
	Actions row { page };
	row.add("NewPrefix", "New prefix", "$FontColorGreen");
	row.add("NewSuffix", "New suffix", "$FontColorGreen");
	row.add("NewCancel", "Cancel", "$FontColorWhite");
	g_book->label(page, "Ask", 82, row.top() + 40, 990, 120, "$FontColorLightGold", "center", 1,
		"A new affix is made in an empty row of the game's table (its number never moves, so recipes and saved items keep pointing at it). Which kind?");
}

bool MadeOpen(const ListView&) { // rows are never removed: Restore puts a made one back as grown
	return false;
}

// The grows asked for under the lock, started now (not under the lock). Any thread.
// ---- stat Disable / Enable / Remove (Live Tables 0.2.0, api version 2): asked for under the lock,
// started by StartCall after it; done() on the UI thread.


void StartCall();

void __cdecl OnCallDone(void*, const livetables::GrowDone* done) noexcept {
	bool ask = false;
	{
		Guard             guard; // the change (Disable / Enable / Shrink / Remove) is applied first
		const StatCall    c   = g_callRunning;
		const bool        ok  = done != nullptr && done->ok != 0;
		const std::string why = done != nullptr && done->why != nullptr ? done->why : "";
		std::string&      note = c.table == livetables::Table::Properties ? g_propsView.note : g_statsView.note;
		RefreshDisabled();
		if (!ok) {
			note = "Not done: " + (why.empty() ? std::string("Live Tables gave no reason.") : why);
		} else if (c.kind == StatCall::SetCells) {
			note = std::to_string(c.count) + (c.count == 1 ? " change" : " changes") + " to '" + c.name + "' saved by Live Tables (it keeps the row, in grown-rows.txt)." + (why.empty() ? "" : " " + why);
			if (c.table == livetables::Table::ItemStatCost && c.newName == "migrate") { // the made stat's line is in Live Tables now: it leaves stat-edits.txt
				std::string kept;
				const std::string failed = BackUpFiles({ StatEditsPath() }, L"before-migration", kept);
				g_statEdits.erase(static_cast<uint16_t>(c.number));
				g_madeStats.insert(c.name);
				WriteMadeStats();
				WriteStatEdits();
				LogF("made stat %s (%d): its cells are kept by Live Tables now; its line left stat-edits.txt (%s)", c.name.c_str(), c.number, failed.empty() ? ("backup: " + kept).c_str() : failed.c_str());
				note = "";
			} else if (c.table == livetables::Table::ItemStatCost) { // a made stat's old line follows the row, until it is handed over (migration)
				const Bank* main = MainBank();
				const auto  line = g_statEdits.find(static_cast<uint16_t>(c.number));
				if (main != nullptr && line != g_statEdits.end() && static_cast<uint64_t>(c.number) < main->stats->count) {
					StatCells cells  = StatRowCells(*main, main->stats->rows + static_cast<size_t>(c.number) * prop::kStatRow);
					cells[kNameCell] = line->second[kNameCell];
					cells[kWasCell]  = line->second[kWasCell];
					line->second     = cells;
					WriteStatEdits();
				}
			}
		} else if (c.kind == StatCall::Rename) {
			note = "'" + c.name + "' is now '" + c.newName + "' (its number stays). Everything that names it follows: the game's name table, Live Tables' files and this plugin's edit files (dated backups first).";
		} else if (c.kind == StatCall::Disable) {
			note = "Stat " + std::to_string(c.number) + " (" + c.name + ") is disabled: no op, no line on items, no item event. Its saved shape stays, so saves keep loading. Enable gives it back.";
			for (const int row : c.props) {
				Command command;
				command.kind = Command::PropOff;
				command.slot = row;
				g_commands.push_back(command);
			}
			if (!c.props.empty()) {
				note += " " + std::to_string(c.props.size()) + (c.props.size() == 1 ? " property that names it is" : " properties that name it are") + " disabled too.";
			}
			ask = true;
		} else if (c.kind == StatCall::Enable) {
			note = "Stat " + std::to_string(c.number) + " (" + c.name + ") is enabled again.";
			if (g_statEdits.count(static_cast<uint16_t>(c.number)) != 0) { // its edited cells back onto the row
				Command command;
				command.kind = Command::StatLine;
				command.slot = c.number;
				g_commands.push_back(command);
				ask = true;
			}
		} else if (c.kind == StatCall::Remove && c.table == livetables::Table::Properties) { // its lines and marks go with it
			g_propsView.card = -1;
			const bool lines = g_edits.erase(c.name) != 0, off = g_off.erase(c.name) != 0;
			g_made.erase(std::remove_if(g_made.begin(), g_made.end(), [&](const Made& m) { return m.code == c.name; }), g_made.end());
			std::string kept;
			std::vector<std::wstring> files { MadePath() };
			if (lines) files.push_back(EditsPath());
			if (off) files.push_back(OffPath());
			const std::string failed = BackUpFiles(files, L"before-remove", kept);
			WriteMade();
			if (lines) WriteEdits();
			if (off) WriteOff();
			note = "Property '" + c.name + "' is removed (row " + std::to_string(c.number) + ")." + (why.empty() ? "" : " " + why);
			LogF("property %s removed: its lines out of the edit files (%s)", c.name.c_str(), failed.empty() ? (kept.empty() ? "nothing to back up" : ("backups: " + kept).c_str()) : failed.c_str());
		} else if (c.kind == StatCall::Remove) {
			g_statsView.card = -1;
			if (g_madeStats.erase(c.name) != 0) {
				WriteMadeStats();
			}
			note = "Stat " + std::to_string(c.number) + " (" + c.name + ") is removed." + (why.empty() ? "" : " " + why);
		}
		LogF("%s %d (%s): %s %s - %s", c.table == livetables::Table::Properties ? "property" : "stat", c.number, c.name.c_str(), c.kind == StatCall::Disable ? "disable" : c.kind == StatCall::Enable ? "enable" : c.kind == StatCall::Rename ? ("rename to " + c.newName).c_str() : c.kind == StatCall::SetCells ? ("cells " + c.cells).c_str() : "remove", ok ? "done" : "NOT done", why.c_str());
		g_callRunning = {};
		BuildViews();
	}
	g_callBusy.store(false);
	StartCall(); // the next one waiting (several cells of a made row, saved at once)
	if (ask) {
		Ask();
	}
	Redraw();
}

// Not under the lock (done() may come at once).
bool LtSetCells() {
	const livetables::Api* lt = g_lt.load();
	return lt != nullptr && lt->version >= 4 && lt->size >= offsetof(livetables::Api, setCells) + sizeof(lt->setCells) && lt->setCells != nullptr;
}

void StartCall() {
	StatCall c;
	{
		Guard guard;
		if (g_callBusy.load()) {
			return;
		}
		if (g_call.kind == StatCall::None && !g_callQueue.empty()) {
			g_call = g_callQueue.front();
			g_callQueue.pop_front();
		}
		if (g_call.kind == StatCall::None) {
			return;
		}
		c             = g_call;
		g_call        = {};
		g_callRunning = c;
	}
	const livetables::Api* lt = g_lt.load();
	if (!LtV2()) {
		Guard guard;
		g_statsView.note = "Disable and Remove need Live Tables 0.2.0 or newer.";
		g_callRunning    = {};
		return;
	}
	g_callBusy.store(true);
	bool started = false;
	if (c.kind == StatCall::Rename || c.kind == StatCall::SetCells) {
		const bool can = c.kind == StatCall::Rename ? lt->version >= 3 && lt->size >= offsetof(livetables::Api, rename) + sizeof(lt->rename) : LtSetCells();
		started        = can && (c.kind == StatCall::Rename ? lt->rename(c.table, 0, c.number, c.newName.c_str(), OnCallDone, nullptr) : lt->setCells(c.table, 0, c.number, c.cells.c_str(), OnCallDone, nullptr)) != 0;
		if (!can) {
			g_callBusy.store(false);
			Guard guard;
			(c.table == livetables::Table::ItemStatCost ? g_statsView : g_propsView).note = c.kind == StatCall::Rename ? "Renaming needs Live Tables 0.3.0 or newer." : "Changing a made row needs Live Tables 0.3.2 or newer.";
			g_callRunning = {};
			return;
		}
	} else {
		auto call = c.kind == StatCall::Disable ? lt->disable : c.kind == StatCall::Enable ? lt->enable : lt->remove;
		started   = call(c.table, 0, c.number, OnCallDone, nullptr) != 0;
	}
	if (!started) {
		g_callBusy.store(false);
		Guard guard;
		g_statsView.note = "Live Tables did not take it.";
		g_callRunning    = {};
	}
}

// The columns of the stat card's 16 cells and the property card's 28, as the tables' headers name them.
constexpr const char* kStatColumns[16] = { "Signed", "Send Bits", "Save Bits", "Save Add", "ValShift", "descpriority", "descfunc", "descval", "descstrpos", "descstrneg", "op", "op param", "op base",
	"op stat1", "op stat2", "op stat3" };

// Live Tables 0.3.2+ keeps made rows' cells (setCells): each made stat's line of stat-edits.txt is
// sent there once, then leaves the file. Live Tables compares save-shape cells with the row in memory
// (equal = only recorded, no save touched). String numbers without a known key are left out (Live
// Tables keeps the cell as it is). Under the lock; started by StartCall.
void MigrateMadeStats() {
	Bank* main = MainBank();
	if (main == nullptr || !LtSetCells()) {
		return;
	}
	for (const auto& [number, cells] : g_statEdits) {
		if (cells[kNameCell].empty() || number >= main->stats->count || GameStatName(*main, number) != cells[kNameCell] || StatDisabledIn(*main, number)) {
			continue; // not a made stat whose row is here (or disabled: Live Tables refuses those)
		}
		std::vector<std::pair<std::string, std::string>> out;
		for (int i = 0; i < kNameCell; ++i) {
			if (i == 0 || i == 2 || i == 3 || i == 4) {
				continue; // Signed, Save Bits, Save Add, ValShift: Live Tables reconciles those itself at its table read (nothing resizes a save here)
			}
			std::string text = cells[static_cast<size_t>(i)];
			if (kFields[i].kind == kCellString && !text.empty() && text[0] >= '0' && text[0] <= '9') {
				const auto key = g_typedKeys.find(static_cast<uint16_t>(atoi(text.c_str())));
				if (key == g_typedKeys.end()) {
					continue; // no key known for this number (5382 from the old filler rows): left as Live Tables keeps it
				}
				text = key->second;
			}
			out.emplace_back(kStatColumns[i], text);
		}
		StatCall c;
		c.kind    = StatCall::SetCells;
		c.number  = number;
		c.name    = cells[kNameCell];
		c.table   = livetables::Table::ItemStatCost;
		c.cells   = ltc::EditText(out);
		c.count   = out.size();
		c.newName = "migrate";
		g_callQueue.push_back(c);
		LogF("made stat %s (%d): its line goes to Live Tables (setCells)", c.name.c_str(), number);
	}
}

// ---- the stat card's "How the game sees it" page and its advanced-stats cell

constexpr uintptr_t kClientUnitsRva = 0x2a23910; // client unit table: type * 0x400, 128 buckets, chain at unit+0x158 (fingerprint 0x97cc2)
constexpr uintptr_t kDescOrderAt    = 0x1278;    // the description order: u16 stat numbers (D2RCore BuildItemStatDescriptionOrder)

// The first player unit of the client unit table (offline: the character in the game). Game thread.
void* PlayerUnit() {
	const auto* table = reinterpret_cast<const uint8_t*>(g_imageBase + kClientUnitsRva);
	if (!Readable(table, 128 * 8)) {
		return nullptr;
	}
	for (int b = 0; b < 128; ++b) {
		void* unit = nullptr;
		memcpy(&unit, table + b * 8, 8);
		for (int steps = 0; unit != nullptr && steps < 64 && Readable(unit, 0x160); ++steps) {
			uint32_t type = 0xffffffff;
			memcpy(&type, unit, 4);
			if (type == 0) {
				return unit;
			}
			memcpy(&unit, static_cast<const uint8_t*>(unit) + 0x158, 8);
		}
	}
	return nullptr;
}

// Game-row advanced-stats edits (stat-adv.txt: name<TAB>value), put back at every table read.
std::map<std::string, int> g_statAdv;
std::wstring StatAdvPath() {
	return g_folder + L"\\stat-adv.txt";
}
void ReadStatAdv() {
	g_statAdv.clear();
	std::string text;
	if (!ReadWhole(StatAdvPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(at, end - at);
		at               = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		const size_t tab = line.find('\t');
		if (!line.empty() && line[0] != '#' && tab != std::string::npos && tab > 0) {
			g_statAdv[line.substr(0, tab)] = atoi(line.c_str() + tab + 1);
		}
	}
}
void WriteStatAdv() {
	std::string text = "# Live Properties: the game's / mod's stats whose advdisplay (Advanced Stats panel) was changed: name, 0 never / 1 always / 2 non-zero. Written by the plugin.\r\n";
	for (const auto& [name, value] : g_statAdv) {
		text += name + "\t" + std::to_string(value) + "\r\n";
	}
	MakeFolders(g_folder);
	static_cast<void>(WriteOwn(StatAdvPath(), text));
}
void ApplyStatAdv(Bank& bank) {
	for (const auto& [name, value] : g_statAdv) {
		for (size_t i = 0; i < bank.statNames.size() && i < bank.stats->count; ++i) {
			if (bank.statNames[i] == name) {
				bank.stats->rows[i * prop::kStatRow + kAdvDisplayAt] = static_cast<uint8_t>(value);
			}
		}
	}
}

// Game thread: a made row through Live Tables (setCells), a game row in memory (every data version) + stat-adv.txt.
void SetAdvDisplay(Bank& main, uint16_t stat, int value) {
	if (stat >= main.stats->count || value < 0 || value > 2) {
		return;
	}
	const std::string name = GameStatName(main, stat);
	static const char* const kAdv[] = { "never", "always", "when not 0" };
	if (WrittenMade(main, stat) && LtSetCells()) {
		StatCall c;
		c.kind   = StatCall::SetCells;
		c.number = stat;
		c.name   = name;
		c.table  = livetables::Table::ItemStatCost;
		c.cells  = "advdisplay=" + std::to_string(value);
		c.count  = 1;
		g_callQueue.push_back(c);
		g_statsView.note = "Advanced stats: " + std::string(kAdv[value]) + " - saving (Live Tables keeps it) ...";
		return;
	}
	for (Bank& bank : g_banks) {
		if (bank.on && stat < bank.stats->count && GameStatName(bank, stat) == name) {
			bank.stats->rows[stat * prop::kStatRow + kAdvDisplayAt] = static_cast<uint8_t>(value);
		}
	}
	const uint8_t orig = (stat + 1u) * prop::kStatRow <= main.statOrig.size() ? main.statOrig[stat * prop::kStatRow + kAdvDisplayAt] : 0;
	if (value == orig) {
		g_statAdv.erase(name);
	} else {
		g_statAdv[name] = value;
	}
	WriteStatAdv();
	g_statsView.note = "Advanced stats: " + std::string(kAdv[value]) + ". The panel shows it the next time it is opened.";
	LogF("stat %d (%s): advdisplay %d", stat, name.c_str(), value);
}

// "How the game sees it": the compiled row as the game holds it, and what it does on the character now. Game thread.
std::string GameView(const Bank& bank, uint16_t stat) {
	if (stat >= bank.stats->count) {
		return "There is no such stat.";
	}
	const uint8_t* row   = bank.stats->rows + stat * prop::kStatRow;
	uint32_t       flags = 0;
	memcpy(&flags, row + prop::kFlags, 4);
	uint16_t base = 0, targets[3] = {}, pos = 0, neg = 0;
	memcpy(&base, row + prop::kOpBase, 2);
	memcpy(targets, row + prop::kOpStat, 6);
	memcpy(&pos, row + prop::kDescStrPos, 2);
	memcpy(&neg, row + prop::kDescStrNeg, 2);
	int32_t saveAdd = 0;
	memcpy(&saveAdd, row + prop::kSaveAdd, 4);
	int16_t priority = 0;
	memcpy(&priority, row + prop::kDescPriority, 2);
	const bool        isSigned = (flags & prop::kFlagSigned) != 0;
	const int         bits = row[prop::kSaveBits], vs = row[prop::kValShift], op = row[prop::kOpAt], param = row[prop::kOpParam];
	const std::string name = StatWord(bank, stat);
	std::string       out  = "Stat " + std::to_string(stat) + " '" + name + "': " + (WrittenMade(bank, stat) ? "made by you (Live Tables keeps it)" : "the game's / mod's own") + (StatOff(stat) ? ", DISABLED" : "") + ".\n";
	if (bits == 0) {
		out += "Saved: not on items (save bits 0).";
	} else {
		const int64_t most = (int64_t(1) << (bits > 40 ? 40 : bits)) - 1;
		out += "Saved: " + std::to_string(bits) + " bits, save add " + std::to_string(saveAdd) + " -> a saved value can be " + std::to_string(-int64_t(saveAdd)) + " .. " + std::to_string(most - saveAdd)
		     + (isSigned ? " (signed)" : "") + "; param bits " + std::to_string(row[prop::kSaveParam]) + "; send bits " + std::to_string(row[prop::kSendBits]) + ".";
	}
	out += vs != 0 ? " Kept x" + std::to_string(1 << vs) + " (1 shown = " + std::to_string(1 << vs) + " inside).\n" : " Kept as shown.\n";
	const std::string target = targets[0] < bank.stats->count ? StatWord(bank, targets[0]) : "-";
	const std::string from   = base < bank.stats->count ? StatWord(bank, base) : "-";
	out += "Op " + std::to_string(op) + ": " + op::Words(op, param, from, target);
	for (int t = 1; t < 3; ++t) {
		out += targets[t] < bank.stats->count && op != 0 ? ", and to " + StatWord(bank, targets[t]) : "";
	}
	out += ".\n";
	std::string adds, from2;
	for (int e = 0; e < prop::kOpListMax; ++e) { // what is added to THIS stat by others (its op list)
		const uint8_t* entry = row + prop::kOpList + e * 6;
		if (entry[4] == 0) {
			break;
		}
		uint16_t b = 0, by = 0;
		memcpy(&b, entry, 2);
		memcpy(&by, entry + 2, 2);
		adds += (adds.empty() ? "" : ", ") + (by < bank.stats->count ? StatWord(bank, by) : "?") + " (op " + std::to_string(entry[4]) + (b < bank.stats->count ? " x " + StatWord(bank, b) : "") + ")";
	}
	for (int d = 0; d < prop::kDependentMax; ++d) { // stats worked out from this one
		uint16_t other = 0;
		memcpy(&other, row + prop::kDependents + d * 2, 2);
		if (other >= bank.stats->count) {
			break;
		}
		from2 += (from2.empty() ? "" : ", ") + StatWord(bank, other);
	}
	out += "Added to it by: " + (adds.empty() ? std::string("nothing") : adds) + ". Worked out from it: " + (from2.empty() ? std::string("nothing") : from2) + ".\n";
	const auto* order = reinterpret_cast<const Vector*>(static_cast<const uint8_t*>(bank.tables) + kDescOrderAt);
	std::string place = "not in it (descfunc 0)";
	if (Readable(order, sizeof(Vector)) && order->count < 0x8000 && Readable(order->rows, order->count * 2)) {
		for (uint64_t i = 0; i < order->count; ++i) {
			uint16_t n = 0;
			memcpy(&n, order->rows + i * 2, 2);
			place = n == stat ? "line " + std::to_string(i + 1) + " of " + std::to_string(order->count) : place;
		}
	}
	const std::string words = StringText(pos);
	out += "Tooltip: descfunc " + std::to_string(row[prop::kDescFunc]) + ", descval " + std::to_string(row[prop::kDescVal]) + ", priority " + std::to_string(priority) + " - " + place
	     + (words.empty() ? std::string(", no text") : ", text \"" + words + "\"") + (neg != pos && neg != 0 ? " / \"" + StringText(neg) + "\"" : "") + ".\n";
	static const char* const kAdv[] = { "never", "always", "when not 0" };
	const int adv = row[kAdvDisplayAt];
	out += "Advanced Stats panel: " + std::string(adv <= 2 ? kAdv[adv] : "?") + ".\n";
	void* unit = g_link.readStat != nullptr ? PlayerUnit() : nullptr;
	if (unit == nullptr) {
		out += "On your character: not in a game (or not found), nothing to show.";
		return out;
	}
	const int32_t value = g_link.readStat(unit, stat, 0);
	out += "On your character now: " + name + " = " + std::to_string(value);
	if (op != 0 && targets[0] < bank.stats->count) {
		const int32_t    b = base < bank.stats->count ? g_link.readStat(unit, base, 0) : 0;
		const int32_t    t = g_link.readStat(unit, targets[0], 0);
		const op::Result r = op::Apply(op, param, value, b, t);
		out += "; " + from + " " + std::to_string(b) + "; " + target + " " + std::to_string(t) + (r.known ? " (of which " + std::to_string(r.add) + " from this: " + r.how + ")" : " (" + r.how + ")");
	}
	out += ".";
	return out;
}

// What names a stat (for the Disable / Remove question): properties with it on a line (their rows,
// for Disable all), stats whose op works from it or on it, and what its own op works on.
std::string StatUses(const Bank& bank, uint16_t stat, std::vector<int>& propRows) {
	std::vector<std::string> props, ops;
	for (size_t i = 0; i < bank.propBase && bank.propRows != nullptr; ++i) {
		for (int line = 0; line < 7; ++line) {
			uint16_t word;
			memcpy(&word, bank.propRows + i * prop::kPropRow + kPropStat + line * 2, 2);
			if (word == stat && bank.propRows[i * prop::kPropRow + kPropFunc + line] != 0) {
				propRows.push_back(static_cast<int>(i));
				props.push_back(PropKey(bank, i));
				break;
			}
		}
	}
	for (size_t i = 0; i < bank.stats->count; ++i) {
		const uint8_t* row = bank.stats->rows + i * prop::kStatRow;
		uint16_t       base, targets[3];
		memcpy(&base, row + prop::kOpBase, 2);
		memcpy(targets, row + prop::kOpStat, 6);
		if (i != stat && row[prop::kOpAt] != 0 && (base == stat || targets[0] == stat || targets[1] == stat || targets[2] == stat)) {
			ops.push_back(StatWord(bank, static_cast<uint16_t>(i)));
		}
	}
	auto list = [](const std::vector<std::string>& words) {
		std::string out;
		for (size_t i = 0; i < words.size() && i < 12; ++i) {
			out += (i == 0 ? "" : ", ") + words[i];
		}
		return out + (words.size() > 12 ? " ..." : "");
	};
	std::string out = props.empty() ? "No property names it." : std::to_string(props.size()) + (props.size() == 1 ? " property names it: " : " properties name it: ") + list(props) + ".";
	out += ops.empty() ? " No other stat's op uses it." : " The op of " + list(ops) + " uses it.";
	return out;
}

// ---- before a Remove: Save Scan's dry run (what the fix would change), on its worker thread
SRWLOCK     g_dryLock = SRWLOCK_INIT; // g_dryText, g_dryReady (Save Scan's thread writes, the game thread reads)
std::string g_dryText;
bool        g_dryReady = false;
int         g_dryStat  = -1;

void __cdecl OnDryFile(void*, const savescan::FileReport* r) noexcept {
	if (r == nullptr || (r->state != savescan::FileState::WouldWrite && r->state != savescan::FileState::Refused)) {
		return;
	}
	AcquireSRWLockExclusive(&g_dryLock);
	if (g_dryText.size() < 600) {
		g_dryText += std::string(g_dryText.empty() ? "" : "; ") + (r->file != nullptr ? r->file : "?") + ": " + std::to_string(r->itemsChanged) + " items ("
		           + std::to_string(r->dropped) + " entries dropped, " + std::to_string(r->renumbered) + " renumbered)"
		           + (r->state == savescan::FileState::Refused ? " - REFUSED: " + std::string(r->why != nullptr ? r->why : "") : "");
	}
	ReleaseSRWLockExclusive(&g_dryLock);
}
void __cdecl OnDryDone(void*, const savescan::FixDone* d) noexcept {
	AcquireSRWLockExclusive(&g_dryLock);
	std::string head;
	if (d == nullptr || d->ok == 0) {
		head = "Save Scan: " + std::string(d != nullptr && d->why != nullptr ? d->why : "no answer") + " ";
	} else if (d->changed == 0) {
		head = "Save Scan read all " + std::to_string(d->files) + " save files: no saved item holds it or a later stat - nothing to fix.";
	} else {
		head = "Save Scan (dry run): " + std::to_string(d->changed) + " of " + std::to_string(d->files) + " save files would be fixed (" + std::to_string(d->itemsChanged) + " items): ";
	}
	g_dryText  = head + g_dryText;
	g_dryReady = true;
	ReleaseSRWLockExclusive(&g_dryLock);
	Ask(); // the game thread's next job puts it on the question
}

// Game thread (Run): the Remove question's findings. Save Scan 2: a dry run of the fix Live Tables
// would ask for; Save Scan 1: the holders; none: said so.
void AskRemove(uint16_t stat) {
	Bank* main = MainBank();
	if (main == nullptr) {
		return;
	}
	const bool        last  = stat + 1u == main->stats->count;
	const std::string fixes = " On Yes, Live Tables has Save Scan fix every save first (a dated backup, write, read back, compare, everything put back on any difference), then removes the row.";
	std::vector<int>  unused;
	std::string       head  = StatUses(*main, stat, unused) + " "
	                   + (last ? "The last stat row: it can be removed in a game too (unless a loaded item holds it)."
	                           : "A middle row: every later stat becomes one lower, in the tables, the saves and the edit files. Only at the main menu (in a game: disable it now).");
	g_statsView.uses = head + fixes + " Save Scan is reading the saves ...";
	if (const savescan::Api* scan2 = savescan::Find(2); scan2 != nullptr) {
		std::vector<savescan::StatChange> changes { { stat, savescan::kDropped } };
		for (size_t n = stat + 1u; n < main->stats->count; ++n) {
			changes.push_back({ static_cast<uint16_t>(n), static_cast<int32_t>(n - 1) });
		}
		AcquireSRWLockExclusive(&g_dryLock);
		g_dryText.clear();
		g_dryReady = false;
		g_dryStat  = stat;
		ReleaseSRWLockExclusive(&g_dryLock);
		savescan::FixRequest request {};
		request.size        = sizeof(request);
		request.flags       = savescan::kDryRun;
		request.changes     = changes.data(); // copied during the call
		request.changeCount = static_cast<uint32_t>(changes.size());
		request.file        = OnDryFile;
		request.done        = OnDryDone;
		g_statsView.uses = head + fixes + (scan2->fixSaves(&request) == 0 ? " Save Scan did not take the dry run." : ""); // the findings follow (TakeDryRun)
		return;
	}
	if (const savescan::Api* scan = savescan::Find(); scan != nullptr) {
		const std::vector<uint16_t> numbers = ltc::RemoveScan(stat, main->stats->count);
		savescan::Summary           sum { sizeof(sum), 0, 0, 0, 0, 0 };
		static_cast<void>(scan->scan(numbers.data(), static_cast<uint32_t>(numbers.size()), &sum, nullptr, nullptr, nullptr));
		g_statsView.uses = head + fixes + " Save Scan " + (sum.ran == 0 ? std::string("could not read the saves.")
		                                                               : "read " + std::to_string(sum.files) + " files: " + std::to_string(sum.holders) + " items hold it or a later stat.");
		return;
	}
	g_statsView.uses = head + " Save Scan is not installed: Live Tables refuses to remove a row then (the saves cannot be checked).";
}

// Game thread: the dry run's findings onto the question. Under the lock.
void TakeDryRun() {
	AcquireSRWLockExclusive(&g_dryLock);
	const bool        ready = g_dryReady;
	const std::string text  = g_dryText;
	const int         stat  = g_dryStat;
	g_dryReady              = false;
	ReleaseSRWLockExclusive(&g_dryLock);
	if (ready && g_statsView.asking && g_statsView.askRemove && g_statsView.card == stat) {
		g_statsView.uses += " " + text;
	}
}

void StartCall();

void StartGrows() {
	StartCall(); // a Disable / Enable / Remove asked for under the lock
	std::vector<ltc::GrowQueue::Item> items;
	std::string                       what;
	{
		Guard guard;
		items.swap(g_toGrow);
		what = g_toGrowWhat;
	}
	const livetables::Api* lt = LT();
	if (items.empty() || lt == nullptr) {
		return;
	}
	auto each = [what](const ltc::GrowQueue::Item& item, const livetables::GrowDone& done) {
		{
			Guard       guard; // applies the grown rows first
			const Bank* main   = MainBank();
			const int   number = main != nullptr && main->version >= 1 && main->version <= 3 ? done.numbers[main->version] : -1;
			const bool  stats  = item.table == livetables::Table::ItemStatCost;
			if (done.ok == 0 || number < 0) {
				const std::string why = done.why != nullptr && done.why[0] != '\0' ? done.why : "no reason given";
				(stats ? g_statsView : g_propsView).note = "Not made: " + why;
				LogF("Live Tables did not grow %s '%s': %s", stats ? "stat" : "property", item.name.c_str(), why.c_str());
			} else if (stats && what == "handover") {
				MoveLine(item.had, static_cast<size_t>(number));
				WriteStatEdits();
				LogF("handover: made stat %s: %zu -> %d (grown by Live Tables; stat-edits.txt rewritten)", item.name.c_str(), item.had, number);
				Command command;
				command.kind = Command::StatLine;
				command.slot = number;
				g_commands.push_back(command);
			} else if (stats && LtSetCells()) { // Live Tables keeps its cells: only its name here
				g_madeStats.insert(item.name);
				WriteMadeStats();
				g_statsView.card = number;
				g_statsView.cell = -1;
				g_statsView.pending.clear();
				g_statsView.note = "Stat " + std::to_string(number) + " (" + item.name + ") is made, at once: 13 bits, may be negative, no text, no op. Live Tables keeps it; give it a line of text and an op here.";
				LogF("made stat %s (%d), grown by Live Tables", item.name.c_str(), number);
			} else if (stats) {
				StatCells cells {};
				cells[0]         = "1";
				cells[1]         = std::to_string(prop::kBits);
				cells[2]         = std::to_string(prop::kBits);
				cells[kNameCell] = item.name;
				cells[kWasCell]  = item.name;
				g_statEdits[static_cast<uint16_t>(number)] = cells;
				WriteStatEdits();
				g_statsView.card = number;
				g_statsView.cell = -1;
				g_statsView.pending.clear();
				g_statsView.note = "Stat " + std::to_string(number) + " (" + item.name + ") is made, at once: 13 bits, may be negative, no text, no op. Give it a line of text and an op here; "
				                   "put it on items through a property (Properties tab: a line with func 1 and this stat).";
				LogF("made stat %s (%d), grown by Live Tables", item.name.c_str(), number);
			} else {
				g_made.push_back({ item.name, item.name });
				WriteMade();
				g_propsView.card    = number;
				g_propsView.cell    = -1;
				g_propsView.naming  = false;
				g_propsView.pending.clear();
				g_propsView.note = "'" + item.name + "' is made, at once (property " + std::to_string(number) + "). Fill its lines and Save; recipes, affixes, uniques and runewords can name it now.";
				LogF("made property %s (%d), grown by Live Tables", item.name.c_str(), number);
			}
			BuildViews();
		}
		Ask(); // a handover's line is put on its row on the game thread
		Redraw();
	};
	auto end = [what](size_t grown, size_t of, const std::string& stopped) {
		if (what == "handover") {
			Guard guard;
			g_handoverNote = grown == of ? "" : "Of " + std::to_string(of) + " made stats without a row, " + std::to_string(grown) + " were handed to Live Tables" + (stopped.empty() ? "" : ": " + stopped) + ".";
			LogF("handover: %zu of %zu made stats grown%s%s", grown, of, stopped.empty() ? "" : " - stopped: ", stopped.c_str());
		}
	};
	if (!g_growQueue.Start(lt, items, each, end)) {
		LogF("Live Tables took no row (%s)", what.c_str());
	}
}

// ---- New property: a code of the user's own, added at once by Live Tables


// Make it: the code is checked, then Live Tables is asked for the row (StartGrows, after the lock).
void MakeProperty(ListView& view) {
	const std::string code = view.nameText;
	Bank*             main = MainBank();
	size_t            row  = 0;
	if (code.empty()) {
		view.note = "Type the new property's code first.";
		return;
	}
	if (main != nullptr && FindRow(*main, code, row)) {
		view.note = "'" + code + "' is a property already: open it in the list, or pick another code.";
		return;
	}
	const livetables::Api* lt = g_lt.load();
	if (lt == nullptr) {
		view.note = kNeedsLiveTables;
		return;
	}
	const std::string text = ltc::NewPropertyRow(code);
	char              why[256] = "";
	if (lt->check(livetables::Table::Properties, 0, text.c_str(), why, sizeof(why)) == 0) {
		view.note = std::string("Not made: ") + why;
		return;
	}
	if (g_growQueue.Busy() || !g_toGrow.empty()) {
		view.note = "Live Tables is still adding a row: try again in a moment.";
		return;
	}
	g_toGrow     = { { livetables::Table::Properties, text, code, 0 } };
	g_toGrowWhat = "new";
	view.naming  = false;
	view.note    = "Adding '" + code + "' (Live Tables) ...";
}

void DrawPropNaming(modbook::Page* page) {
	const ListView& v = g_propsView;
	Actions         row { page };
	row.add("NameYes", "Make it", "$FontColorGreen");
	row.add("NameNo", "Cancel", "$FontColorWhite");
	g_book->title(page, "New property");
	g_book->label(page, "Ask", 82, row.top() + 20, 990, 200, "$FontColorLightGold", "left", 1,
		"Type the new property's code (letters, digits and - % / _ + . ; up to 31), then Enter or Make it. It is the word recipes, affixes, uniques and runewords name it by, "
		"like res-all or dmg%. Live Tables adds its row at once; then fill its lines on its card.");
	g_book->image(page, "CodeSlot", 210, row.top() + 240, "PANEL\\Modals\\InputSlot");
	g_book->label(page, "Code", 250, row.top() + 268, 660, 56, "$FontColorWhite", "left", 0, (v.nameText + "_").c_str());
	g_book->label(page, "Note", 82, row.top() + 360, 990, 200, "$FontColorLightRed", "left", 1, v.note.c_str());
}

// Stats > New: a stat named stat<n> (n = the number it gets), asked of Live Tables.
void MakeStat(ListView& view) {
	const livetables::Api* lt   = g_lt.load();
	const Bank*            main = MainBank();
	if (lt == nullptr || main == nullptr) {
		view.note = kNeedsLiveTables;
		return;
	}
	if (g_growQueue.Busy() || !g_toGrow.empty()) {
		view.note = "Live Tables is still adding a row: try again in a moment.";
		return;
	}
	const size_t number = main->stats->count;
	std::string  name   = "stat" + std::to_string(number);
	uint16_t     other  = 0;
	for (int k = 2; StatNumber(*main, name, other); ++k) {
		name = "stat" + std::to_string(number) + "_" + std::to_string(k);
	}
	const std::string text = ltc::NewStatRow(name);
	char              why[256] = "";
	if (lt->check(livetables::Table::ItemStatCost, 0, text.c_str(), why, sizeof(why)) == 0) {
		view.note = std::string("Not made: ") + why;
		return;
	}
	g_toGrow     = { { livetables::Table::ItemStatCost, text, name, 0 } };
	g_toGrowWhat = "new";
	view.note    = "Adding " + name + " (Live Tables) ...";
}

void __cdecl TabDraw(void* user, modbook::Page* page) noexcept {
	Guard guard;
	if (user == &g_affixView) {
		if (g_affixView.card >= 0) {
			DrawAffixCard(page);
		} else if (g_affixChoosing) {
			DrawAffixChoice(page);
		} else {
			DrawList(page, g_affixView, "Type to search: prefix, scha, made", "(click to open)", "New affix");
		}
	} else if (user == &g_funcsView) {
		DrawList(page, g_funcsView, "Type to search: skill, damage", "(click: puts its number into the func cell you are typing in)", nullptr);
	} else if (user == &g_statsView) {
		if (g_statsView.card >= 0) {
			DrawStatCard(page);
		} else {
			DrawList(page, g_statsView, "Type to search: maxhp, x256, empty", "(click: opens it - or, with a property's stat cell open, puts its name in)", "New stat");
		}
	} else if (g_propsView.card >= 0) {
		DrawCard(page);
	} else if (g_propsView.naming) {
		DrawPropNaming(page);
	} else {
		DrawList(page, g_propsView, "Type to search: res-all, func 21, maxhp", "(click to open)", "New property");
	}
}

void QueueEdit(Command::Kind kind, int row, int cell, const std::string& text) {
	Command command;
	command.kind    = kind;
	command.slot    = row;
	command.percent = cell;
	command.a       = text;
	if (g_inventory != nullptr) {
		static_cast<void>(g_inventory->getLocalPlayer(g_ctx, &command.player));
	}
	g_commands.push_back(command);
}

std::string UnsavedNote(const ListView& view) {
	return std::to_string(view.pending.size()) + (view.pending.size() == 1 ? " change is" : " changes are") + " not saved: Save changes, or Discard.";
}

// What a card's cell of the row holds (nothing typed counted). Under the lock.
std::string RowValue(const ListView& view, int cell) {
	const Bank* bank = MainBank();
	if (bank == nullptr || view.card < 0 || cell < 0) {
		return {};
	}
	if (&view == &g_affixView) {
		return bank->affixOn && static_cast<uint64_t>(view.card) < bank->affixes->count && cell < affix::kCells ? AffixCells(*bank, static_cast<size_t>(view.card))[static_cast<size_t>(cell)] : "";
	}
	if (&view == &g_propsView) {
		if (cell == kCodeCell) {
			return static_cast<uint64_t>(view.card) < bank->propBase ? PropKey(*bank, static_cast<size_t>(view.card)) : "";
		}
		return static_cast<uint64_t>(view.card) < bank->propBase && cell < kCells ? RowCells(*bank, bank->propRows + static_cast<size_t>(view.card) * prop::kPropRow)[static_cast<size_t>(cell)] : "";
	}
	if (static_cast<uint64_t>(view.card) >= bank->stats->count || cell >= kStatCells) {
		return {};
	}
	if (cell == kNameCell) {
		const auto edited = g_statEdits.find(static_cast<uint16_t>(view.card));
		return edited != g_statEdits.end() ? edited->second[kNameCell] : "";
	}
	return StatRowCells(*bank, bank->stats->rows + static_cast<size_t>(view.card) * prop::kStatRow)[static_cast<size_t>(cell)];
}

// The card's typing rules (card_core): every typed cell stays pending until Save or Discard,
// whether the player leaves it with Enter, Tab, a click on another cell or Save. Under the lock.
template <class Step> void Typed(ListView& view, Step step) {
	card::Typing t;
	t.cell    = view.cell;
	t.text    = view.cellText;
	t.pending = view.pending;
	step(t, [&view](int cell) { return RowValue(view, cell); });
	view.cell     = t.cell;
	view.cellText = t.text;
	view.pending  = std::move(t.pending);
}

// Has the open card's row been changed or made (so Restore has something to do)? Under the lock.
bool Edited(const ListView& view) {
	const Bank* bank = MainBank();
	if (bank == nullptr || view.card < 0) {
		return false;
	}
	if (&view == &g_statsView) {
		return g_statEdits.count(static_cast<uint16_t>(view.card)) != 0;
	}
	if (&view == &g_affixView) {
		if (!bank->affixOn || static_cast<uint64_t>(view.card) >= bank->affixes->count) {
			return false;
		}
		int    kind  = 0;
		size_t index = 0;
		AffixPlace(*bank, static_cast<size_t>(view.card), kind, index);
		return g_affixEdits.count(AffixKey(kind, index)) != 0;
	}
	return bank->props->rows == bank->propRows && static_cast<uint64_t>(view.card) < bank->props->count && g_edits.count(PropKey(*bank, static_cast<size_t>(view.card))) != 0;
}

// Is the Properties card's row disabled? Under the lock.
bool OffNow(const ListView& view) {
	const Bank* bank = MainBank();
	return &view == &g_propsView && bank != nullptr && view.card >= 0 && static_cast<uint64_t>(view.card) < bank->propBase && g_off.count(PropKey(*bank, static_cast<size_t>(view.card))) != 0;
}

// The Legend on the other side (Mod Book 0.8.0) asks whether a cell is being typed in, and puts an
// entry's text into it.
int __cdecl TabEditing(void* user) noexcept {
	const auto* view = static_cast<const ListView*>(user);
	Guard       guard;
	return view != &g_funcsView && view->card >= 0 && view->cell >= 0 ? 1 : 0;
}
int __cdecl TabInsert(void* user, const char* text) noexcept {
	auto* view = static_cast<ListView*>(user);
	{
		Guard guard;
		if (view == &g_funcsView || view->card < 0 || view->cell < 0 || text == nullptr) {
			return 0;
		}
		view->cellText = std::string(text).substr(0, 48);
	}
	Redraw();
	return 1;
}


// Save on a made row's card (a stat or property Live Tables grew): the typed cells leave `all` and go
// to Live Tables as one setCells (it keeps the row, compiles it the game's way, asks Save Scan for a
// save-shape change). The name / code cell stays in `all` (a rename). Under the lock. False: not a
// made row (or no setCells): `all` goes the usual way. `why`: a refusal, nothing sent.
bool MadeRowCells(ListView& view, std::map<int, std::string>& all, std::string& why) {
	const Bank* bank = MainBank();
	if (bank == nullptr || view.card < 0 || !LtSetCells()) {
		return false;
	}
	const bool stats = &view == &g_statsView;
	std::string name;
	if (stats) {
		if (static_cast<uint64_t>(view.card) >= bank->stats->count || !WrittenMade(*bank, static_cast<uint16_t>(view.card))) {
			return false;
		}
		name = GameStatName(*bank, static_cast<uint16_t>(view.card));
	} else {
		if (static_cast<uint64_t>(view.card) >= bank->propBase || MadeByCode(PropKey(*bank, static_cast<size_t>(view.card))) == nullptr) {
			return false;
		}
		name = PropKey(*bank, static_cast<size_t>(view.card));
	}
	std::vector<std::pair<std::string, std::string>> cells;
	for (auto it = all.begin(); it != all.end();) {
		const int   cell = it->first;
		std::string text = it->second == "-" ? "" : it->second;
		if ((stats && cell == kNameCell) || (!stats && cell == kCodeCell)) {
			++it;
			continue;
		}
		std::string column;
		if (stats && cell >= 0 && cell < kNameCell) {
			column = kStatColumns[cell];
			if (kFields[cell].kind == kCellString && !text.empty() && text[0] >= '0' && text[0] <= '9') { // a number: its key, if one was typed for it
				const auto key = g_typedKeys.find(static_cast<uint16_t>(atoi(text.c_str())));
				if (key == g_typedKeys.end()) {
					why = std::string(kFields[cell].name) + ": type the string's key (Live Tables keeps keys, not numbers). Nothing was saved.";
					return true;
				}
				text = key->second;
			}
		} else if (!stats && cell >= 0 && cell < kCells) {
			static const char* const parts[] = { "func", "stat", "set", "val" };
			column = std::string(parts[cell % 4]) + std::to_string(cell / 4 + 1);
		} else {
			++it;
			continue;
		}
		cells.emplace_back(column, text);
		it = all.erase(it);
	}
	if (cells.empty()) {
		return true;
	}
	StatCall c;
	c.kind   = StatCall::SetCells;
	c.number = view.card;
	c.name   = name;
	c.table  = stats ? livetables::Table::ItemStatCost : livetables::Table::Properties;
	c.cells  = ltc::EditText(cells);
	c.count  = cells.size();
	g_callQueue.push_back(c);
	view.note = "Saving " + std::to_string(cells.size()) + (cells.size() == 1 ? " change" : " changes") + " to '" + name + "' (Live Tables keeps it) ...";
	return true;
}

void __cdecl TabClick(void* user, const char* what) noexcept {
	bool  ask = false, close = false;
	auto* view = static_cast<ListView*>(user);
	const bool stats = view == &g_statsView, cards = view != &g_funcsView, affixes = view == &g_affixView;
	{
		Guard             guard;
		const std::string name = what != nullptr ? what : "";
		const bool props = view == &g_propsView;
		const Bank*    bank = MainBank();
		const uint16_t stat = static_cast<uint16_t>(view->card);
		const bool     card = stats && bank != nullptr && view->card >= 0 && static_cast<uint64_t>(view->card) < bank->stats->count;
		const bool affixCard = affixes && bank != nullptr && bank->affixOn && view->card >= 0 && static_cast<uint64_t>(view->card) < bank->affixes->count;
		std::string affixKey;
		if (affixCard) {
			int    k = 0;
			size_t i = 0;
			AffixPlace(*bank, static_cast<size_t>(view->card), k, i);
			affixKey = AffixKey(k, i);
		}
		const bool affixOff = affixCard && g_affixOff.count(affixKey) != 0;
		if (affixes && view->asking && (name == "Escape" || name == "OffNo" || name == "Back")) {
			view->asking = false;
		} else if (affixes && view->asking && affixCard && name == "OffYes") {
			view->asking = false;
			QueueEdit(Command::AffixOff, view->card, 0, "");
			ask = true;
		} else if (affixes && view->asking) {
			// only the question's own buttons while it asks
		} else if (affixCard && name == "AffixOff" && (!view->pending.empty() || view->cell >= 0)) {
			view->note = "Save or Discard the typed cells first.";
		} else if (affixCard && name == "AffixOff" && affixOff) {
			QueueEdit(Command::AffixOn, view->card, 0, ""); // enabling gives back what was there: no question
			ask = true;
		} else if (affixCard && name == "AffixOff") {
			view->asking = true;
		} else if (affixOff && (name.rfind("Cell", 0) == 0 || name == "Save" || name == "Restore" || name == "CubeOnly" || name == "RareDrops" || name == "NormalDrops")) {
			view->note = affixKey + " is disabled: Enable it first, then change it.";
		} else if (card && name == "GameView") {
			view->gameView = !view->gameView;
			view->gameText.clear();
			if (view->gameView) {
				QueueEdit(Command::StatGameView, view->card, 0, ""); // read on the game thread
				ask = true;
			}
		} else if (card && name == "AdvCycle") {
			const int adv = bank->stats->rows[stat * prop::kStatRow + kAdvDisplayAt];
			QueueEdit(Command::StatAdv, view->card, adv >= 2 ? 0 : adv + 1, "");
			if (view->gameView) {
				QueueEdit(Command::StatGameView, view->card, 0, "");
			}
			ask = true;
		} else if (stats && view->asking && (name == "Escape" || name == "OffNo" || name == "Back")) {
			view->asking = false;
			view->askProps.clear();
		} else if (stats && view->asking && card && (name == "OffYes" || name == "OffAll")) {
			g_call        = { StatCall::Disable, view->card, StatWord(*bank, stat), name == "OffAll" ? view->askProps : std::vector<int>() };
			view->asking  = false;
			view->askProps.clear();
			view->note = "Disabling " + g_call.name + " (Live Tables) ...";
		} else if (stats && view->asking && card && name == "RemoveYes") {
			g_call       = { StatCall::Remove, view->card, StatWord(*bank, stat), {} };
			view->asking = false;
			view->note   = "Removing " + g_call.name + " (Live Tables, Save Scan) ...";
		} else if (stats && view->asking) {
			// only the question's own buttons while it asks
		} else if (card && (name == "StatOff" || name == "StatRemove") && (!view->pending.empty() || view->cell >= 0)) {
			view->note = "Save or Discard the typed cells first.";
		} else if (card && name == "StatOff" && (g_callBusy.load() || g_call.kind != StatCall::None)) {
			view->note = "Live Tables is still working on the last one: try again in a moment.";
		} else if (card && name == "StatOff" && StatOff(stat)) {
			g_call     = { StatCall::Enable, view->card, StatWord(*bank, stat), {} };
			view->note = "Enabling " + g_call.name + " (Live Tables) ...";
		} else if (card && name == "StatOff") {
			view->askProps.clear();
			view->uses      = StatUses(*bank, stat, view->askProps);
			view->asking    = true;
			view->askRemove = false;
		} else if (card && name == "StatRemove") {
			view->uses      = "Save Scan is reading the saves ...";
			view->asking    = true;
			view->askRemove = true;
			QueueEdit(Command::StatRemoveAsk, view->card, 0, ""); // Save Scan on the game thread
			ask = true;
		} else if (card && StatOff(stat) && (name.rfind("Cell", 0) == 0 || name == "Save" || name == "Restore")) {
			view->note = "This stat is disabled: Enable it first, then change it.";
		} else if (props && view->naming && (name == "Escape" || name == "NameNo")) {
			view->naming = false;
		} else if (props && view->naming && name == "NameYes") {
			MakeProperty(*view);
		} else if (props && view->naming) {
			// only the question's own buttons while it asks
		} else if (props && view->card < 0 && name == "New") {
			view->note.clear();
			if (LT() == nullptr) {
				view->note = kNeedsLiveTables;
			} else {
				view->naming = true;
				view->nameText.clear();
			}
		} else if (props && view->asking && (name == "Escape" || name == "OffNo" || name == "Back")) {
			view->asking = false;
		} else if (props && view->asking && view->askRemove && name == "OffYes") { // Remove it (a made property): Live Tables takes the row out
			view->asking = false;
			if (bank != nullptr && static_cast<uint64_t>(view->card) < bank->propBase) {
				g_call        = {};
				g_call.kind   = StatCall::Remove;
				g_call.number = view->card;
				g_call.name   = PropKey(*bank, static_cast<size_t>(view->card));
				g_call.table  = livetables::Table::Properties;
				view->note    = "Removing '" + g_call.name + "' (Live Tables) ...";
			}
		} else if (props && view->asking && name == "OffYes") {
			view->asking = false;
			QueueEdit(Command::PropOff, view->card, 0, "");
			ask = true;
		} else if (props && view->asking) {
			// only the question's own buttons while it asks
		} else if (props && view->card >= 0 && name == "PropRemove") {
			if (!view->pending.empty() || view->cell >= 0) {
				view->note = "Save or Discard the typed cells first, then Remove.";
			} else if (g_callBusy.load() || g_call.kind != StatCall::None) {
				view->note = "Live Tables is still working on the last one: try again in a moment.";
			} else if (bank != nullptr) {
				view->uses      = PropUses(*bank, static_cast<size_t>(view->card), PropKey(*bank, static_cast<size_t>(view->card)))
				           + " Live Tables removes only the LAST property row it grew, and refuses while an affix, cube recipe, unique, runeword or a table read at load names the code.";
				view->asking    = true;
				view->askRemove = true;
			}
		} else if (props && view->card >= 0 && name == "Off") {
			const Bank* bank = MainBank();
			if (bank == nullptr || static_cast<uint64_t>(view->card) >= bank->propBase) {
				view->note = "That row cannot be changed here.";
			} else if (g_off.count(PropKey(*bank, static_cast<size_t>(view->card))) != 0) {
				QueueEdit(Command::PropOn, view->card, 0, ""); // enabling gives back what was there: no question
				ask = true;
			} else if (!view->pending.empty() || view->cell >= 0) {
				view->note = "Save or Discard the typed cells first, then Disable.";
			} else {
				view->uses      = PropUses(*bank, static_cast<size_t>(view->card), PropKey(*bank, static_cast<size_t>(view->card)));
				view->asking    = true;
				view->askRemove = false;
			}
		} else if (props && view->card >= 0 && name == "Restore" && !OffNow(*view) && MadeOpen(*view)) { // removing a made property asks first
			const Bank* bank = MainBank();
			if (!view->pending.empty() || view->cell >= 0) {
				view->note = "Save or Discard the typed cells first, then Remove.";
			} else if (bank != nullptr) {
				view->uses      = PropUses(*bank, static_cast<size_t>(view->card), PropKey(*bank, static_cast<size_t>(view->card)));
				view->asking    = true;
				view->askRemove = true;
			}
		} else if (props && view->card >= 0 && (name.rfind("Cell", 0) == 0 || name == "Save" || name == "Restore") && OffNow(*view)) {
			view->note = "This property is disabled: Enable it first, then change it.";
		} else if (affixes && view->card < 0 && g_affixChoosing && (name == "Escape" || name == "NewCancel")) {
			g_affixChoosing = false;
		} else if (affixes && view->card < 0 && g_affixChoosing && (name == "NewPrefix" || name == "NewSuffix")) {
			// the first empty row of that kind (not row 0: pre=0 / suf=0 means none)
			const Bank* bank = MainBank();
			const int   kind = name == "NewPrefix" ? kPrefixes : kSuffixes;
			size_t      row  = 0;
			bool        found = false;
			std::string held;
			for (size_t index = 1; bank != nullptr && !found && AffixRowOf(*bank, kind, index, row); ++index) {
				found = affix::IsBlank(bank->affixes->rows + row * affix::kRow, bank->names) && g_affixEdits.count(AffixKey(kind, index)) == 0;
				std::string why;
				if (found && !AffixIdKnown(*bank, kind, index, why)) { // a saved item would hold an id not confirmed for this row
					held  = why;
					found = false;
				}
			}
			g_affixChoosing = false;
			if (!found) {
				view->note = held.empty() ? std::string("There is no empty ") + kAffixKinds[kind] + " row left." : "Not made: " + held;
			} else {
				int    k = 0;
				size_t index = 0;
				AffixPlace(*bank, row, k, index);
				view->card = static_cast<int>(row);
				view->cell = -1;
				view->pending.clear();
				view->pending[affix::kNameCell]      = "New affix";
				view->pending[affix::kSpawnCell]     = "1";
				view->pending[affix::kLevelCell]     = "99";
				view->pending[affix::kFrequencyCell] = "1";
				view->note = AffixKey(kind, index) + " was empty and is now yours to fill (Cube only is filled in): give it a name, a mod and the item types (scha for small charms), then Save changes.";
			}
		} else if (affixes && view->card >= 0 && (name == "CubeOnly" || name == "RareDrops" || name == "NormalDrops")) {
			view->cell = -1;
			view->pending[affix::kSpawnCell] = "1";
			if (name == "CubeOnly") {
				view->pending[affix::kFrequencyCell] = "1";
				view->pending[affix::kLevelCell]     = "99";
				view->pending[affix::kMaxLevelCell]  = "";
				view->note = "Cube only: level 99. An item's affix level is at most 99 and drops seldom reach it, so give the recipe lvl 99 (its output's item level). Save changes to keep it.";
			} else if (name == "RareDrops") {
				view->pending[affix::kFrequencyCell] = "1";
				view->note = "Rare drops: frequency 1, the lowest that still lets a recipe force it; it can also drop, seldom. Save changes to keep it.";
			} else {
				view->pending[affix::kFrequencyCell] = "4";
				view->note = "Normal drops: frequency 4, like most of the game's affixes. Save changes to keep it.";
			}
		} else if (name == "Escape") {
			if (cards && view->cell >= 0) {
				view->cell = -1;
			} else if (cards && view->card >= 0 && !view->pending.empty()) {
				view->note = UnsavedNote(*view);
			} else if (cards && view->card >= 0) {
				view->card = -1;
			} else {
				close = true;
			}
		} else if (name.rfind("Row", 0) == 0 && view->card < 0) {
			const size_t at = view->top + static_cast<size_t>(atoi(name.c_str() + 3));
			if (at < view->found.size() && view->entries[view->found[at]].row >= 0) { // row -1: a note, not a row
				const Entry& entry  = view->entries[view->found[at]];
				ListView&    props  = g_propsView;
				const bool   typing = props.card >= 0 && props.cell >= 0;
				if (view == &g_propsView || affixes) {
					view->card = entry.row;
					view->cell = -1;
					view->note.clear();
				} else if (typing && props.cell % 4 == (stats ? 1 : 0)) {
					props.cellText = entry.word; // into the property's stat (or func) cell
				} else if (stats) {
					view->card = entry.row;
					view->cell = -1;
					view->note.clear();
				}
			}
		} else if (cards && name == "Back" && !view->pending.empty()) {
			view->note = UnsavedNote(*view);
		} else if (cards && name == "Back") {
			view->card = view->cell = -1;
			view->gameView = false;
		} else if (cards && name == "Save" && view->card >= 0 && view->pending.empty() && view->cell < 0) {
			view->note = "Nothing to save: type in a cell first.";
		} else if (cards && name == "Save" && view->card >= 0) {
			std::map<int, std::string> all;
			Typed(*view, [&](card::Typing& t, const card::Saved& saved) { all = t.TakeAll(saved); }); // the cell being typed in counts too
			std::string why;
			const size_t typed = all.size();
			const bool   made  = !affixes && MadeRowCells(*view, all, why); // a made row: its cells go to Live Tables (one setCells); a name / code cell stays (rename)
			for (const auto& [cell, text] : all) {
				QueueEdit(affixes ? Command::AffixEdit : stats ? Command::StatEdit : Command::Edit, view->card, cell, text);
			}
			if (!why.empty()) {
				view->note = why;
			} else if (!made || typed == 0) {
				view->note = typed == 0 ? "Nothing changed: every typed cell holds what the row holds." : std::to_string(all.size()) + (all.size() == 1 ? " change" : " changes") + " sent to the game.";
			}
			ask        = !all.empty();
		} else if (cards && name == "Discard") {
			Typed(*view, [](card::Typing& t, const card::Saved&) { t.Discard(); });
			view->note = "Changes discarded.";
		} else if (cards && name == "Restore" && view->card >= 0 && !Edited(*view)) {
			view->note = "This row is the game's own: there is nothing to restore.";
		} else if (cards && name == "Restore" && view->card >= 0) {
			view->pending.clear();
			QueueEdit(affixes ? Command::AffixRestore : stats ? Command::StatRestore : Command::Restore, view->card, 0, "");
			view->cell = -1;
			ask        = true;
		} else if (affixes && name == "New" && view->card < 0) {
			g_affixChoosing = true;
			view->note.clear();
		} else if (stats && name == "New" && view->card < 0) {
			LT();
			MakeStat(*view);
		} else if (cards && name.rfind("Cell", 0) == 0 && view->card >= 0) {
			int at = atoi(name.c_str() + 4);
			at     = at >= 0 && at < (affixes ? affix::kCells : stats ? kStatCells : kCells + 1) ? at : -1; // properties: + the code cell
			Typed(*view, [at](card::Typing& t, const card::Saved& saved) { t.Open(at, saved); }); // the cell being left keeps its text
		}
	}
	StartGrows(); // New stat / New property: Live Tables is asked now, not under the lock
	if (ask) {
		Ask();
	}
	if (close && g_book != nullptr) {
		g_book->close(modbook::SideOf(g_book, view->handle, view->side)); // the player may have dragged the tab to the other side
	}
	Redraw();
}

void __cdecl TabKeys(void* user, const char* typed) noexcept {
	bool  ask  = false;
	auto* view = static_cast<ListView*>(user);
	const bool stats = view == &g_statsView;
	{
		Guard guard;
		for (const char* at = typed; at != nullptr && *at != '\0'; ++at) {
			const char c = *at;
			if (view == &g_propsView && view->naming) { // the new property's code
				if (c == modbook::kKeyBackspace) {
					if (!view->nameText.empty()) {
						view->nameText.pop_back();
					}
				} else if (c == modbook::kKeyEnter) {
					MakeProperty(*view);
					break;
				} else if (CodeLetter(c) && view->nameText.size() < kCodeMost) {
					view->nameText += c;
				}
				continue;
			}
			const bool   inCard = view->card >= 0;
			std::string& text   = inCard ? view->cellText : view->words;
			if (inCard && view->cell < 0) {
				continue;
			}
			if (c == modbook::kKeyBackspace) {
				if (!text.empty()) {
					text.pop_back();
				}
			} else if (inCard && (c == modbook::kKeyEnter || c == modbook::kKeyTab)) {
				view->note.clear();
				const int next = c == modbook::kKeyTab && view->cell + 1 < (view == &g_affixView ? affix::kCells : stats ? kNameCell : kCells) ? view->cell + 1 : -1;
				Typed(*view, [next](card::Typing& t, const card::Saved& saved) { t.Next(next, saved); }); // kept until Save changes
			} else if (static_cast<unsigned char>(c) >= 0x20 && c != modbook::kKeyDelete && text.size() < 48 && c != '"' && c != '\\') {
				text += c;
			}
			if (!inCard) {
				view->top = 0;
				Filter(*view);
			}
		}
	}
	StartGrows(); // Enter on the new property's code
	if (ask) {
		Ask();
	}
	Redraw();
}

void __cdecl TabMove(void* user, int pages, int rows, int jump) noexcept {
	auto* view = static_cast<ListView*>(user);
	{
		Guard guard;
		if (view->card >= 0) {
			return;
		}
		const long long count = static_cast<long long>(view->found.size());
		long long       top   = static_cast<long long>(view->top) + pages * static_cast<long long>(kListRows) + rows;
		const long long last  = count > static_cast<long long>(kListRows) ? count - static_cast<long long>(kListRows) : 0;
		top                   = jump < 0 ? 0 : jump > 0 ? last : top;
		view->top             = static_cast<size_t>(top < 0 ? 0 : top > last ? last : top);
	}
	Redraw();
}

void __cdecl TabShown(void*, int) noexcept {}
int __cdecl  TabLeave(void* user) noexcept {
	auto* view = static_cast<ListView*>(user);
	Guard guard;
	if (view->card >= 0 && !view->pending.empty()) {
		view->note = UnsavedNote(*view);
		return 0; // the card stays, saying why
	}
	view->asking = false; // a question left open is not answered
	view->naming = false;
	return 1;
}

// Puts the tabs into the Mod Book, once it is there. Any thread; not under the lock.
void BookSetUp() {
	static std::atomic<bool> done { false };
	if (!g_enabled || done.load()) {
		return;
	}
	const modbook::Api* book = modbook::Find();
	if (book == nullptr || done.exchange(true)) {
		return;
	}
	g_statsView.side = g_funcsView.side = modbook::kRight;
	g_propsView.side = g_affixView.side = modbook::kLeft;
	const modbook::Tab props { sizeof(modbook::Tab), modbook::kLeft, "properties", "Properties", &g_propsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr, TabEditing, TabInsert };
	const modbook::Tab stats { sizeof(modbook::Tab), modbook::kRight, "stats", "Stats", &g_statsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties", TabEditing, TabInsert };
	const modbook::Tab funcs { sizeof(modbook::Tab), modbook::kRight, "funcs", "Funcs", &g_funcsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties", TabEditing, TabInsert };
	g_propsView.handle = book->addTab(&props);
	g_statsView.handle = book->addTab(&stats);
	g_funcsView.handle = book->addTab(&funcs);
	const modbook::Tab affixes { sizeof(modbook::Tab), modbook::kLeft, "affixes", "Affixes", &g_affixView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr, TabEditing, TabInsert };
	g_affixView.handle = book->addTab(&affixes);
	g_book             = book;
	if (modbook::HasLegend(book)) {
		static const char* const ids[kLegendParts]    = { "props", "stats", "affixes" };
		static const char* const titles[kLegendParts] = { "Properties", "Stats", "Affixes" };
		const int                follows[kLegendParts] = { g_propsView.handle, g_statsView.handle, g_affixView.handle };
		for (int part = 0; part < kLegendParts; ++part) {
			const modbook::LegendSection section { sizeof(modbook::LegendSection), ids[part], titles[part], follows[part], &g_legend[part], LegendCount, LegendGet };
			g_legendHandle[part] = book->addLegend(&section);
		}
		LogF("Mod Book Legend: sections Properties, Stats, Affixes %s", g_legendHandle[0] != 0 && g_legendHandle[1] != 0 && g_legendHandle[2] != 0 ? "added" : "NOT all added, see mod-book.log");
	} else {
		LogF("Mod Book before 0.8.0: no Legend, no action row (the buttons are drawn the old way)");
	}
	LogF("Mod Book: tabs Properties and Affixes (left, %s, %s), Stats and Funcs (right, %s, %s)", g_propsView.handle != 0 ? "added" : "REFUSED, see mod-book.log", g_affixView.handle != 0 ? "added" : "REFUSED",
		g_statsView.handle != 0 ? "added" : "REFUSED", g_funcsView.handle != 0 ? "added" : "REFUSED");
}

// ---------------------------------------------------------------- threads

void __cdecl SayJob(const D2RL::PluginContext*, void*) noexcept {
	std::vector<std::string> lines;
	{
		Guard guard;
		lines.swap(g_say);
	}
	for (const std::string& line : lines) {
		g_ctx->WriteConsoleMessage(line.c_str());
	}
}

void FlushSay() {
	if (g_threads == nullptr || g_threads->runOnUiThread(g_ctx, SayJob, nullptr) != D2RL::Threads::Result::Success) {
		Guard guard;
		g_say.clear(); // no console to write to; the lines are in the log
	}
}

void __cdecl Job(const D2RL::PluginContext*, void*) noexcept {
	{
		Guard guard;
		if (!g_wanted.exchange(false)) {
			return;
		}
		if (!g_setUp) {
			SetUp();
		}
		std::vector<Command> commands;
		commands.swap(g_commands);
		for (const Command& command : commands) {
			Run(command);
		}
		TakeDryRun();
		if ((g_disabledDirty && RefreshDisabled()) || !commands.empty()) {
			BuildViews();
		}
	}
	StartGrows();
	FlushSay();
	Redraw();
}

// A job asked for on the game's thread does not run while no game runs. This one goes round on
// the UI's thread and runs Job there when it has waited 1.5 s in vain.
void __cdecl Later(const D2RL::PluginContext*, void*) noexcept {
	if (!g_wanted.load()) {
		return;
	}
	if (GetTickCount64() - g_askedMs.load() > 1500 || g_threads->runOnUiThread(g_ctx, Later, nullptr) != D2RL::Threads::Result::Success) {
		Job(g_ctx, nullptr);
	}
}

void Ask() {
	g_wanted.store(true);
	g_askedMs.store(GetTickCount64());
	if (g_threads->runOnGameThread(g_ctx, Job, nullptr) != D2RL::Threads::Result::Success) {
		if (g_threads->runOnUiThread(g_ctx, Job, nullptr) != D2RL::Threads::Result::Success) {
			Job(g_ctx, nullptr);
		}
	} else {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, Later, nullptr));
	}
}

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	{
		Guard guard;
		SetUp();
	}
	StartGrows(); // the handover to Live Tables, if SetUp found made stats without a row
	BookSetUp();
	FlushSay();
	Redraw();
}

// ---------------------------------------------------------------- console

constexpr char kUsage[] = "Live Properties: prop book (Properties) | prop stats | prop funcs | prop affixes - the tabs in the Mod Book (Ctrl+B / Ctrl+L) | prop affixtest - a test prefix on a ring, to confirm saved affix ids";

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	if (!g_enabled || !g_link.ok || g_threads == nullptr) {
		g_ctx->WriteConsoleMessage(!g_enabled ? "Live Properties: off in the settings (enabled = false)." : ("Live Properties: off - " + g_linkProblem + ".").c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	char              word[16] = "";
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	const int         got = sscanf(args.c_str(), " %15s", word);
	const std::string w   = got == 1 ? word : "";
	if (w == "affixtest") {
		{
			Guard guard;
			Command command;
			command.kind = Command::AffixTest;
			g_commands.push_back(command);
		}
		Ask();
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (w == "book" || w == "stats" || w == "funcs" || w == "affixes") {
		BookSetUp();
		if (g_book == nullptr) {
			g_ctx->WriteConsoleMessage("Live Properties: the Mod Book plugin (d2rl-mod-book.dll) is not installed; there are no panels without it.");
		} else {
			g_book->toggle(w == "book" ? g_propsView.handle : w == "funcs" ? g_funcsView.handle : w == "affixes" ? g_affixView.handle : g_statsView.handle);
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	g_ctx->WriteConsoleMessage(kUsage);
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "live-properties",
	.name        = "Live Properties",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Proof of concept: item properties of the kind 'N% of stat A is added to stat B', made and changed while a game runs.",
	.flags       = static_cast<D2RL::PluginFlags>(static_cast<uint32_t>(D2RL::PluginFlags::Shared) | static_cast<uint32_t>(D2RL::PluginFlags::NativeHooks)), // NativeHooks: editNativeItem
	.reserved    = {},
};

// The sha256 of this plugin's own DLL file (Windows CNG), for the start line: what ran.
std::string OwnSha256() {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH];
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnSha256), &self) || GetModuleFileNameW(self, path, MAX_PATH) == 0) {
		return "unknown (no path)";
	}
	std::string bytes;
	if (!ReadWhole(path, bytes)) {
		return "unknown (not readable)";
	}
	BCRYPT_ALG_HANDLE alg = nullptr;
	uint8_t           digest[32] {};
	const bool        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
	               && BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) == 0;
	if (alg != nullptr) {
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	if (!ok) {
		return "unknown (no hash)";
	}
	char hex[65];
	for (int i = 0; i < 32; ++i) {
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	}
	return hex;
}

// liveprops::Api::findProperty: the game's own codes and the ones made this session.
int __cdecl FindProperty(const char* code, int dataVersion) noexcept {
	if (code == nullptr || code[0] == '\0') {
		return -1;
	}
	Guard       guard;
	const Bank* bank = dataVersion >= 1 && dataVersion <= 3 ? &g_banks[dataVersion - 1] : MainBank();
	size_t      row  = 0;
	return bank != nullptr && bank->on && FindRow(*bank, code, row) ? static_cast<int>(row) : -1;
}

// liveprops::Api::listMade: the rows of ours the game really read in this start (SetUp checks each
// against the game's own name table), in row order.
size_t __cdecl ListMade(liveprops::Table table, int dataVersion, liveprops::MadeRow* out, size_t room) noexcept {
	Guard       guard;
	const Bank* bank = dataVersion >= 1 && dataVersion <= 3 ? &g_banks[dataVersion - 1] : MainBank();
	if (bank == nullptr || !bank->on || (table != liveprops::Table::stats && table != liveprops::Table::properties)) {
		return 0;
	}
	const bool               stats = table == liveprops::Table::stats;
	std::vector<std::string> names; // what this plugin made: the made names of stat-edits.txt, the codes of made-properties.txt
	if (stats) {
		for (const auto& [number, cells] : g_statEdits) {
			names.push_back(cells[kNameCell]);
		}
		names.insert(names.end(), g_madeStats.begin(), g_madeStats.end());
	} else {
		for (const Made& made : g_made) {
			names.push_back(made.code);
		}
	}
	// the names walked from the game's name tables, which ApplyChanges (Guard) has just brought up to
	// date: no Live Tables call, so any thread (Live Export asks from its own worker thread)
	auto find = [&](const std::string& name) { return ltc::IndexOf(stats ? bank->statNames : bank->propNames, name); };
	std::vector<std::pair<std::string, int>> rows = ltc::Listed(names, find);
	if (stats) { // only a line whose number is its row's (a line not yet moved by the handover is left out), or one Live Tables keeps
		rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& r) {
			if (g_madeStats.count(r.first) != 0) {
				return false;
			}
			const auto l = g_statEdits.find(static_cast<uint16_t>(r.second));
			return l == g_statEdits.end() || l->second[kNameCell] != r.first;
		}), rows.end());
	}
	for (size_t i = 0; out != nullptr && i < rows.size() && i < room; ++i) {
		liveprops::MadeRow& row = out[i];
		memset(&row, 0, sizeof(row));
		strncpy(row.name, rows[i].first.c_str(), sizeof(row.name) - 1);
		row.number      = rows[i].second;
		row.dataVersion = bank->version;
	}
	return rows.size();
}

const liveprops::Api g_api { sizeof(liveprops::Api), liveprops::kApiVersion, FindProperty, ListMade };

} // namespace

// Live Tables adds the rows of itemstatcost.txt / properties.txt from now on: this plugin reads
// neither table's text any more (live_tables_api.h).
extern "C" __declspec(dllexport) const int LivePropsUsesLiveTables = 1;

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) {
		return false;
	}
	g_ctx = context;
	LoadConfig();
	static_cast<void>(context->RegisterConsoleCommand("prop", OnConsole, "Live Properties: make, change or switch off a live item property (type prop for the list)"));
	if (!g_enabled) {
		LogF("Live Properties %s: off in the settings (enabled = false)", kVersion);
		return true;
	}
	if (!LinkGame()) {
		LogF("Live Properties %s: off - %s", kVersion, g_linkProblem.c_str());
		return true;
	}
	LogF("table rows: Live Tables adds them (this plugin no longer reads the tables' text; New needs Live Tables)");
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads     = nullptr;
		g_link.ok     = false;
		g_linkProblem = "this D2RLoader has no thread service";
		LogF("Live Properties %s: off - %s", kVersion, g_linkProblem.c_str());
		return true;
	}
	if (context->QueryService(&g_items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(g_items, offsetof(D2RL::ItemService, createItem) + sizeof(void*))) {
		g_items = nullptr;
	}
	if (context->QueryService(&g_inventory) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasInventoryServiceField(g_inventory, offsetof(D2RL::InventoryService, getLocalPlayer) + sizeof(void*))) {
		g_inventory = nullptr;
	}
	const D2RL::ResourceService* resources = nullptr;
	if (context->QueryService(&resources) == D2RL::ServiceQueryResult::Success && D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
		RegisterStrings(resources);
	} else {
		LogF("text: this D2RLoader has no resource service; items will show no line");
	}
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, registerGameplayEventListener))) {
		D2RL::Lifecycle::DataTablesLoadedListener tables {};
		tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
		tables.callback   = OnTablesLoaded;
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
	}
	BookSetUp(); // Mod Book may load later; then this happens when the tables are read
	// stay loaded: the game's property table points into memory this plugin made
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Job), &self);
	LogF("Live Properties %s: d2rl-live-properties.dll, sha256 %s", kVersion, OwnSha256().c_str());
	LogF("Live Properties %s: on - type prop in the console (item service %s, D2R %s)", kVersion, g_items != nullptr && g_inventory != nullptr ? "yes" : "NO: prop give will not work",
		context->buildVersion != nullptr ? context->buildVersion : "?");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	// The tables stay as they are: the memory they point into is never freed.
}

D2RL_PLUGIN_EXPORT const liveprops::Api* __cdecl LivePropsGetApi(uint32_t version) noexcept {
	return version >= 1 ? &g_api : nullptr; // the newest to any caller (it checks api->version / size; a version 1 caller reads only findProperty)
}
