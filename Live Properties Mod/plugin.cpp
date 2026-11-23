// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Properties: the game's item tables changed while a game runs, in the game's memory. No
// table reload, no restart, the mod's own files are never written.
//
// Tabs in the Mod Book:
//   Properties  every row of the property table; a row's card changes its func / stat / set / val
//   Stats       every stat; a card changes a row, New makes a stat of an empty row (Exalted Items'
//               xlt_unused_ fillers), the op linking done the game's own way (prop_core.h)
//   Funcs       the property funcs the game has
//   Affixes     every magic prefix / suffix (affix_core.h); New makes one of an empty row
// The changes are kept in d2rloader\live-suite\live-properties\<mod>\ (property-edits.txt,
// stat-edits.txt, affix-edits.txt) and put back every time the game has read its tables.
//
// Console (Ctrl + `): prop | prop book | prop stats | prop funcs | prop affixes
//

#include <D2RLPlugin/api.h>

#include "prop_core.h"

#include "../Mod Book/mod_book_api.h"
#include "../Save Scan/save_scan_api.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "affix_core.h"
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.7.0";

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
	enum Kind { Edit, Restore, StatEdit, StatRestore, StatNew, AffixEdit, AffixRestore } kind = Edit; // Edit: slot = property row, percent = cell, a = text
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

struct Guard {
	Guard() { AcquireSRWLockExclusive(&g_lock); }
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

// Names for affixes that are not one of the game's string keys: Name1..Name16, kNameTextRoom
// letters each, written over like the property lines.
constexpr int kNameSlots = 16, kNameTextRoom = 40;
char*         g_nameStrings[kNameSlots] = {};
uint16_t      g_nameIds[kNameSlots]     = {};
std::string   g_nameText[kNameSlots];        // what each holds now ("" = free)
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

// Finds the game's copies of the eight strings. Each must still be what the plugin gave (the
// placeholder, or a text this plugin wrote earlier in the same place).
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
		char*             at  = id != unknown ? const_cast<char*>(g_link.stringById(id)) : nullptr;
		const bool        was = g_nameStrings[slot - 1] != nullptr && g_nameStrings[slot - 1] == at;
		if (at == nullptr || !Readable(at, kNameTextRoom + 1) || (!was && memcmp(at, NamePlaceholder(slot).c_str(), kNameTextRoom + 1) != 0)) {
			g_nameStrings[slot - 1] = nullptr;
			g_nameIds[slot - 1]     = 0;
			continue;
		}
		g_nameStrings[slot - 1] = at;
		g_nameIds[slot - 1]     = id;
		++names;
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
using Cells          = std::array<std::string, kCells>;
constexpr size_t kPropSet = 0x02, kPropVal = 0x0a, kPropFunc = 0x18, kPropStat = 0x20;

std::map<std::string, Cells> g_edits; // the changed rows, by property code ("#12" for a row without one)

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

std::string StatWord(const Bank& bank, uint16_t stat) {
	if (stat == prop::kNoStat) {
		return {};
	}
	if (const auto made = g_statEdits.find(stat); made != g_statEdits.end() && !made->second[kNameCell].empty() && (stat >= bank.statNames.size() || bank.statNames[stat].empty() || bank.statNames[stat].rfind("xlt_unused_", 0) == 0)) {
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

void ApplyStatEdits(Bank& bank) {
	int done = 0;
	for (const auto& [stat, cells] : g_statEdits) {
		std::string problem;
		if (stat >= bank.stats->count || GameStatName(bank, stat) != cells[kWasCell]) {
			if (bank.version == 3) {
				Say("stat-edits.txt: stat %d is not '%s' in the game's table any more; that line does nothing", stat, cells[kWasCell].empty() ? "(an empty row)" : cells[kWasCell].c_str());
			}
		} else if (!ApplyStat(bank, stat, cells, D2RL::InvalidPlayerHandle, false, problem, nullptr)) {
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
		if (g_nameStrings[i] != nullptr && g_nameText[i].empty()) {
			bool taken = false;
			for (const auto& [other, used] : g_nameSlotOf) {
				taken = taken || used == i;
			}
			slot = taken ? -1 : i;
		}
	}
	if (slot < 0 || g_nameStrings[slot] == nullptr) {
		problem = "'" + text + "' is not one of the game's names, and the plugin's " + std::to_string(kNameSlots) + " names of its own are all in use";
		return false;
	}
	g_nameSlotOf[rowKey] = slot;
	g_nameText[slot]     = text;
	memcpy(g_nameStrings[slot], text.c_str(), text.size() + 1);
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
	bank.names.props     = bank.propNames.data();
	bank.names.propCount = bank.propNames.size();
	bank.names.types     = bank.typeCodes.data();
	bank.names.typeCount = bank.typeCodes.size();
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

void SetUp() {
	g_setUp  = true;
	g_folder = OwnFolder();
	if (!g_moved.empty()) {
		LogF("your files were %s", g_moved.c_str());
		g_moved.clear();
	}
	ReadEdits();
	ReadStatEdits();
	ReadAffixEdits();
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
		ApplyEdits(bank);
		ApplyStatEdits(bank);
		if (AffixSetUp(bank)) {
			ApplyAffixEdits(bank);
		}
		LogF("data version %d: on - %d op stats checked, %llu stats, %llu properties", bank.version, seen, static_cast<unsigned long long>(bank.stats->count),
			static_cast<unsigned long long>(bank.propBase));
	}
	for (const Bank& bank : g_banks) {
		if (!bank.on) {
			LogF("data version %d: off - %s", bank.version, bank.off.c_str());
		}
	}
	BuildViews();
}

void Run(const Command& command) {
	Bank* main = MainBank();
	if (main == nullptr) {
		Say("off - %s", !g_link.ok ? g_linkProblem.c_str() : g_banks[2].off.c_str());
		return;
	}
	if (command.kind == Command::Edit || command.kind == Command::Restore) {
		RunEdit(command);
	} else if (command.kind == Command::StatEdit || command.kind == Command::StatRestore || command.kind == Command::StatNew) {
		RunStat(command);
	} else if (command.kind == Command::AffixEdit || command.kind == Command::AffixRestore) {
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
};
ListView g_statsView, g_propsView, g_funcsView, g_affixView;
bool     g_affixChoosing = false; // the Affixes tab asks: a new prefix or a new suffix?
constexpr size_t kListRows = 9;

// What each func does: the game's data guide's words. Which funcs exist is read from the game.
const char* FuncWords(int func) {
	static const char* const words[] = { "nothing", "adds the rolled value to the stat (set = 1: on set items)", "like 1, for defense-type stats", "the same roll as the line above, to another stat",
		"like 3 (the data guide calls it unused; this game has a handler for it)", "minimum damage", "maximum damage", "damage %", "speed stats (attack, cast, hit recovery...)",
		"the same param and roll as the line above", "skill tab bonus", "skill on an event (chance to cast)", "a random param from the min-max range", "durability", "sockets", "uses min only", "uses max only",
		"uses the param only", "by time of day", "charged skill", "a yes/no stat (indestructible)", "a group of skills; val = which", "one skill; the param names it", "ethereal",
		"a state on the character or the target", "no words in the data guide; this game has a handler for it (not looked into)" };
	return func >= 0 && func < static_cast<int>(sizeof(words) / sizeof(words[0])) ? words[func] : func == 36 ? "a random skill of a class" : "no words for it";
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
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* row = bank->stats->rows + i * prop::kStatRow;
			Entry          entry;
			entry.row         = static_cast<int>(i);
			const auto edited = g_statEdits.find(static_cast<uint16_t>(i));
			const bool named  = i < bank->statNames.size() && !bank->statNames[i].empty() && bank->statNames[i].rfind("xlt_unused_", 0) != 0;
			entry.word        = StatWord(*bank, static_cast<uint16_t>(i));
			entry.title       = std::to_string(i) + "  " + (entry.word) + (edited == g_statEdits.end() ? "" : named ? "   (changed)" : "   (made by you)");
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
		for (size_t i = 0; i < bank->props->count && bank->props->rows == bank->propRows; ++i) {
			const uint8_t* row = bank->propRows + i * prop::kPropRow;
			Entry          entry;
			entry.row = static_cast<int>(i);
			{
				const std::string key   = PropKey(*bank, i);
				const Cells       cells = RowCells(*bank, row);
				entry.title             = std::to_string(i) + "  " + key + (g_edits.count(key) != 0 ? "   (changed)" : "");
				for (int line = 0; line < 7; ++line) {
					const std::string& func = cells[static_cast<size_t>(line * 4)];
					if (func.empty()) {
						continue;
					}
					entry.detail += (entry.detail.empty() ? "" : ";  ") + ("func " + func) + (cells[static_cast<size_t>(line * 4 + 1)].empty() ? "" : " " + cells[static_cast<size_t>(line * 4 + 1)])
					              + (cells[static_cast<size_t>(line * 4 + 2)].empty() ? "" : " set " + cells[static_cast<size_t>(line * 4 + 2)])
					              + (cells[static_cast<size_t>(line * 4 + 3)].empty() ? "" : " val " + cells[static_cast<size_t>(line * 4 + 3)]);
				}
				if (entry.detail.empty()) {
					entry.detail = "does nothing (no func)";
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
			entry.title = AffixKey(kind, index) + "  " + (cells[affix::kNameCell].empty() ? "(no name)" : cells[affix::kNameCell])
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
	for (ListView* view : { &g_statsView, &g_propsView, &g_funcsView, &g_affixView }) {
		Filter(*view);
		if (view->card >= static_cast<int>(view->entries.size())) {
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
		LogF("property %s (row %zu): back to the game's own row", key.c_str(), row);
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

// A stat's cell changed (StatEdit), a stat back to the game's own row - for a made stat that
// removes it - (StatRestore), or a new stat made of an empty row (StatNew). Game thread, under the lock.
void RunStat(const Command& command) {
	std::string& note = g_statsView.note;
	Bank*        main = MainBank();
	if (main == nullptr) {
		note = "the game's tables are not there";
		return;
	}
	uint16_t  stat = static_cast<uint16_t>(command.slot);
	StatCells cells;
	if (command.kind == Command::StatNew) {
		int free = -1;
		for (int n = static_cast<int>(main->stats->count) - 1; n >= 0 && free < 0; --n) {
			if (static_cast<uint64_t>(n) < main->stats->count && prop::IsBlank(main->stats->rows + n * prop::kStatRow) && g_statEdits.count(static_cast<uint16_t>(n)) == 0
				&& (GameStatName(*main, static_cast<uint16_t>(n)).empty() || GameStatName(*main, static_cast<uint16_t>(n)).rfind("xlt_unused_", 0) == 0)) {
				free = n;
			}
		}
		if (free < 0) {
			note = "Not made: the table has no empty row left (Exalted Items' xlt_unused_ rows are the ones that can be taken).";
			return;
		}
		stat             = static_cast<uint16_t>(free);
		cells            = StatRowCells(*main, main->stats->rows + stat * prop::kStatRow);
		cells[0]         = "1";
		cells[1]         = std::to_string(prop::kBits);
		cells[2]         = std::to_string(prop::kBits);
		cells[kNameCell] = "stat" + std::to_string(stat);
	} else if (stat >= main->stats->count || (stat + 1u) * prop::kStatRow > main->statOrig.size()) {
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
		if (command.percent == kNameCell && !GameStatName(*main, stat).empty() && GameStatName(*main, stat).rfind("xlt_unused_", 0) != 0) {
			note = "Not changed: the game names this stat itself.";
			return;
		}
		cells[static_cast<size_t>(command.percent)] = command.a == "-" ? "" : command.a;
	}
	const std::string was = GameStatName(*main, stat);
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
	if (command.kind == Command::StatRestore || (same && name.empty())) {
		g_statEdits.erase(stat);
	} else {
		g_statEdits[stat] = cells;
	}
	WriteStatEdits();
	const std::string who = "Stat " + std::to_string(stat) + " (" + StatWord(*main, stat) + ")";
	if (command.kind == Command::StatNew) {
		g_statsView.card = stat;
		g_statsView.cell = -1;
		note = who + " is made: 13 bits, may be negative, no text, no op. Give it a name, a line of text and an op here; put it on items through a property (Properties tab: a line with func 1 and this stat).";
	} else if (command.kind == Command::StatRestore) {
		note = who + " is the game's own row again.";
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
	if (g_book != nullptr) {
		g_book->redraw(g_statsView.handle);
		g_book->redraw(g_propsView.handle);
		g_book->redraw(g_funcsView.handle);
	}
}

void DrawList(modbook::Page* page, const ListView& view, const char* hint, const char* tip, bool newButton) {
	g_book->image(page, "SearchSlot", 210, 138, "PANEL\\Modals\\InputSlot");
	g_book->label(page, "Search", 250, 166, 660, 56, view.words.empty() ? "$FontColorGoldYellow" : "$FontColorWhite", "left", 0, view.words.empty() ? hint : (Cut(view.words, 34) + "_").c_str());
	if (newButton) {
		g_book->button(page, "New", 86, 172, 116, "New", "$FontColorGreen");
	}
	const int top = 268;
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

// Save changes / Discard, while a card has cells typed and not saved (or one being typed).
void SaveButtons(modbook::Page* page, const ListView& v) {
	const size_t count = v.pending.size() + (v.cell >= 0 && v.pending.count(v.cell) == 0 ? 1 : 0);
	if (count != 0) {
		g_book->button(page, "Save", 214, 172, 300, ("Save changes (" + std::to_string(count) + ")").c_str(), "$FontColorGreen");
		g_book->button(page, "Discard", 530, 172, 200, "Discard", "$FontColorLightRed");
	}
}

// One property row: its 7 lines of func / stat / set / val, each cell a place to type in.
void DrawCard(modbook::Page* page) {
	const ListView& v    = g_propsView;
	const Bank*     bank = MainBank();
	g_book->button(page, "Back", 82, 172, 116, "Back", "$FontColorWhite");
	if (bank == nullptr || bank->props->rows != bank->propRows || static_cast<uint64_t>(v.card) >= bank->props->count) {
		g_book->label(page, "Gone", 82, 300, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	const size_t      row  = static_cast<size_t>(v.card);
	const bool        own  = row < bank->propBase;
	const std::string key  = PropKey(*bank, row);
	const Cells       cells = RowCells(*bank, bank->propRows + row * prop::kPropRow);
	g_book->title(page, ("Property " + std::to_string(row) + ": " + key).c_str());
	if (own && g_edits.count(key) != 0) {
		g_book->button(page, "Restore", 742, 172, 330, "The game's own row", "$FontColorLightRed");
	}
	SaveButtons(page, v);
	static const char* const columns[] = { "func", "stat", "set", "val" };
	static const char* const tips[]    = { "func: what the line does (a number from the Funcs tab; empty = the line does nothing)", "stat: the stat the line works on (a name from the Stats tab, or its number)",
		"set: 1 on the lines the game marks so; leave it as the game has it unless you know the func", "val: a fixed number some funcs use (func 21: which group of skills)" };
	const int xs[] = { 130, 290, 780, 900 }, widths[] = { 140, 470, 100, 170 };
	for (int c = 0; c < 4; ++c) {
		g_book->label(page, (std::string("Head") + columns[c]).c_str(), xs[c], 268, widths[c], 36, "$FontColorGoldYellow", "left", 0, columns[c]);
	}
	for (int line = 0; line < 7; ++line) {
		const int         y = 316 + line * 96;
		const std::string n = std::to_string(line);
		g_book->label(page, ("Line" + n).c_str(), 82, y, 40, 36, "$FontColorGoldYellow", "left", 0, std::to_string(line + 1).c_str());
		for (int c = 0; c < 4; ++c) {
			const int         cell = line * 4 + c;
			const std::string name = "Cell" + std::to_string(cell);
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
		g_book->label(page, ("Words" + n).c_str(), 130, y + 40, 940, 36, "$FontColorGoldYellow", "left", 0, func != 0 ? (std::string("func ") + std::to_string(func) + ": " + FuncWords(func)).c_str() : "");
	}
	g_book->label(page, "Note", 82, 1000, 990, 100, "$FontColorGreen", "left", 1, v.note.c_str());
	g_book->label(page, "Help", 82, 1110, 990, 180, "$FontColorGoldYellow", "left", 1,
		own ? "Click a cell, type, Enter keeps it (Tab: keeps it and goes to the next cell, Esc: leaves it); kept cells show green with a *. Save changes sends them to the game. With a func or stat cell open, a click in the Funcs or Stats tab puts it in. "
		      "Saved changes count for items made from then on; items that exist keep the stats they have."
		    : "");
}

// One stat row: 16 cells of it, and the plugin's name for a stat the game has no name for.
void DrawStatCard(modbook::Page* page) {
	const ListView& v    = g_statsView;
	const Bank*     bank = MainBank();
	g_book->button(page, "Back", 82, 172, 116, "Back", "$FontColorWhite");
	if (bank == nullptr || v.card < 0 || static_cast<uint64_t>(v.card) >= bank->stats->count) {
		g_book->label(page, "Gone", 82, 300, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	const uint16_t  stat   = static_cast<uint16_t>(v.card);
	const uint8_t*  row    = bank->stats->rows + stat * prop::kStatRow;
	const bool      live   = false;
	const auto      edited = g_statEdits.find(stat);
	const bool      named  = !GameStatName(*bank, stat).empty() && GameStatName(*bank, stat).rfind("xlt_unused_", 0) != 0;
	StatCells       cells  = StatRowCells(*bank, row);
	cells[kNameCell]       = edited != g_statEdits.end() ? edited->second[kNameCell] : "";
	g_book->title(page, ("Stat " + std::to_string(stat) + ": " + StatWord(*bank, stat)).c_str());
	if (edited != g_statEdits.end()) {
		g_book->button(page, "Restore", 742, 172, 330, named ? "The game's own row" : "Remove this stat", "$FontColorLightRed");
	}
	SaveButtons(page, v);
	static const char* const groups[] = { "saved", "text", "op" };
	for (int i = 0; i < kStatCells; ++i) {
		if (i == kNameCell && named) {
			continue;
		}
		const int         y      = 268 + i * 54;
		const std::string n      = std::to_string(i);
		const bool        typing = i == v.cell;
		const auto        typed  = v.pending.find(i);
		const std::string now    = typed != v.pending.end() ? typed->second + "*" : cells[static_cast<size_t>(i)];
		const std::string text   = typing ? v.cellText + "_" : now.empty() ? "-" : now;
		std::string       hint   = i == kNameCell ? "your name for it (used in the lists and in stat cells)" : groups[kFields[i].group];
		if (i < kNameCell && kFields[i].kind == kCellString) {
			const std::string words = StringText(static_cast<uint16_t>(FieldValue(row, kFields[i])));
			hint                    = words.empty() ? hint : "\"" + words + "\"";
		}
		g_book->label(page, ("Name" + n).c_str(), 82, y, 300, 36, "$FontColorGoldYellow", "left", 0, i == kNameCell ? "name" : kFields[i].name);
		g_book->label(page, ("Text" + n).c_str(), 390, y, 330, 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0, Cut(text, 19).c_str());
		g_book->label(page, ("Hint" + n).c_str(), 730, y, 340, 36, "$FontColorGoldYellow", "left", 0, Cut(hint, 20).c_str());
		if (!live) {
			g_book->spot(page, ("Spot" + n).c_str(), 82, y, 980, 50, ("Cell" + n).c_str(),
				i == kNameCell ? "The plugin's own name for a stat you made. The game does not know it; this plugin's lists and cells do." : kFields[i].tip);
		}
	}
	g_book->label(page, "Note", 82, 268 + kStatCells * 54 + 8, 990, 130, "$FontColorGreen", "left", 1,
		v.note.empty() ? "Click a line, type, Enter keeps it (Tab: next line, Esc: leaves it); kept lines show green with a *. Save changes sends them to the game. Hover a line for what it means. 'saved' lines are refused while you carry an item with the stat." : v.note.c_str());
}

// One affix row: 35 cells in two columns, the three ways to keep a made affix (off drops or not), and the recipe word.
void DrawAffixCard(modbook::Page* page) {
	const ListView& v    = g_affixView;
	const Bank*     bank = MainBank();
	g_book->button(page, "Back", 82, 172, 116, "Back", "$FontColorWhite");
	if (bank == nullptr || !bank->affixOn || v.card < 0 || static_cast<uint64_t>(v.card) >= bank->affixes->count) {
		g_book->label(page, "Gone", 82, 300, 990, 36, "$FontColorLightRed", "left", 0, "The game's tables are not there right now.");
		return;
	}
	int    kind  = 0;
	size_t index = 0;
	AffixPlace(*bank, static_cast<size_t>(v.card), kind, index);
	const affix::Cells cells  = AffixCells(*bank, static_cast<size_t>(v.card));
	const auto         edited = g_affixEdits.find(AffixKey(kind, index));
	g_book->title(page, (std::string(kind == kPrefixes ? "Prefix " : kind == kSuffixes ? "Suffix " : "Automagic ") + std::to_string(index) + (cells[affix::kNameCell].empty() ? "" : ": " + cells[affix::kNameCell])).c_str());
	if (edited != g_affixEdits.end()) {
		g_book->button(page, "Restore", 742, 172, 330, edited->second.was.empty() ? "Remove this affix" : "The game's own row", "$FontColorLightRed");
	}
	SaveButtons(page, v);
	constexpr int kPerColumn = 18;
	for (int i = 0; i < affix::kCells; ++i) {
		const bool         right  = i >= kPerColumn;
		const int          y      = 268 + (right ? i - kPerColumn : i) * 48;
		const int          x      = right ? 585 : 82;
		const std::string  n      = std::to_string(i);
		const bool         typing = i == v.cell;
		const auto         typed  = v.pending.find(i);
		const std::string  now    = typed != v.pending.end() ? typed->second + "*" : cells[static_cast<size_t>(i)];
		const std::string  text   = typing ? v.cellText + "_" : now.empty() ? "-" : now;
		g_book->label(page, ("Name" + n).c_str(), x, y, 170, 36, "$FontColorGoldYellow", "left", 0, affix::kFields[i].name);
		g_book->label(page, ("Text" + n).c_str(), x + 175, y, right ? 310 : 300, 36, typing ? "$FontColorWhite" : typed != v.pending.end() ? "$FontColorGreen" : "$FontColorLightGold", "left", 0,
			Cut(text, i == affix::kNameCell ? 17 : 16).c_str());
		g_book->spot(page, ("Spot" + n).c_str(), x, y, 480, 44, ("Cell" + n).c_str(), affix::kFields[i].tip);
	}
	g_book->button(page, "CubeOnly", 82, 1140, 300, "Cube only", "$FontColorLightTeal");
	g_book->button(page, "RareDrops", 412, 1140, 300, "Rare drops", "$FontColorLightTeal");
	g_book->button(page, "NormalDrops", 742, 1140, 300, "Normal drops", "$FontColorLightTeal");
	const std::string word = kind == kPrefixes ? "pre=" + std::to_string(index) : kind == kSuffixes ? "suf=" + std::to_string(index) : "";
	g_book->label(page, "Note", 82, 1196, 990, 100, v.note.empty() ? "$FontColorGoldYellow" : "$FontColorGreen", "left", 1,
		(!v.note.empty() ? v.note
		                 : (word.empty() ? std::string("Automagic rows are the affixes an item base gives itself (items.txt auto prefix).")
		                                 : "In a recipe: " + word + " (with mag), e.g. cm1,mag," + word + ". The game takes it only if spawnable is 1, frequency above 0, its level reached and the item's type listed.")
		                       + " The three buttons fill the cells for you; Save changes sends them.")
			.c_str());
}

// The Affixes tab before a New: which kind.
void DrawAffixChoice(modbook::Page* page) {
	g_book->label(page, "Ask", 82, 320, 990, 120, "$FontColorLightGold", "center", 1,
		"A new affix is made in an empty row of the game's table (its number never moves, so recipes and saved items keep pointing at it). Which kind?");
	g_book->button(page, "NewPrefix", 82, 480, 300, "New prefix", "$FontColorGreen");
	g_book->button(page, "NewSuffix", 412, 480, 300, "New suffix", "$FontColorGreen");
	g_book->button(page, "NewCancel", 742, 480, 300, "Cancel", "$FontColorWhite");
}

void __cdecl TabDraw(void* user, modbook::Page* page) noexcept {
	Guard guard;
	if (user == &g_affixView) {
		if (g_affixView.card >= 0) {
			DrawAffixCard(page);
		} else if (g_affixChoosing) {
			DrawAffixChoice(page);
		} else {
			DrawList(page, g_affixView, "Type to search: prefix, scha, made", "(click to open)", true);
		}
	} else if (user == &g_funcsView) {
		DrawList(page, g_funcsView, "Type to search: skill, damage", "(click: puts its number into the func cell you are typing in)", false);
	} else if (user == &g_statsView) {
		if (g_statsView.card >= 0) {
			DrawStatCard(page);
		} else {
			DrawList(page, g_statsView, "Type to search: maxhp, x256, empty", "(click: opens it - or, with a property's stat cell open, puts its name in)", true);
		}
	} else if (g_propsView.card >= 0) {
		DrawCard(page);
	} else {
		DrawList(page, g_propsView, "Type to search: res-all, func 21, maxhp", "(click to open)", false);
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

// What a card's cell holds now (a typed, not saved one first). Under the lock.
std::string CellNow(const ListView& view) {
	const Bank* bank = MainBank();
	if (bank == nullptr || view.card < 0 || view.cell < 0) {
		return {};
	}
	if (const auto typed = view.pending.find(view.cell); typed != view.pending.end()) {
		return typed->second;
	}
	if (&view == &g_affixView) {
		return bank->affixOn && static_cast<uint64_t>(view.card) < bank->affixes->count && view.cell < affix::kCells ? AffixCells(*bank, static_cast<size_t>(view.card))[static_cast<size_t>(view.cell)] : "";
	}
	if (&view == &g_propsView) {
		return static_cast<uint64_t>(view.card) < bank->propBase && view.cell < kCells ? RowCells(*bank, bank->propRows + static_cast<size_t>(view.card) * prop::kPropRow)[static_cast<size_t>(view.cell)] : "";
	}
	if (static_cast<uint64_t>(view.card) >= bank->stats->count || view.cell >= kStatCells) {
		return {};
	}
	if (view.cell == kNameCell) {
		const auto edited = g_statEdits.find(static_cast<uint16_t>(view.card));
		return edited != g_statEdits.end() ? edited->second[kNameCell] : "";
	}
	return StatRowCells(*bank, bank->stats->rows + static_cast<size_t>(view.card) * prop::kStatRow)[static_cast<size_t>(view.cell)];
}

void __cdecl TabClick(void* user, const char* what) noexcept {
	bool  ask = false, close = false;
	auto* view = static_cast<ListView*>(user);
	const bool stats = view == &g_statsView, cards = view != &g_funcsView, affixes = view == &g_affixView;
	{
		Guard             guard;
		const std::string name = what != nullptr ? what : "";
		if (affixes && view->card < 0 && g_affixChoosing && (name == "Escape" || name == "NewCancel")) {
			g_affixChoosing = false;
		} else if (affixes && view->card < 0 && g_affixChoosing && (name == "NewPrefix" || name == "NewSuffix")) {
			// the first empty row of that kind (not row 0: pre=0 / suf=0 means none)
			const Bank* bank = MainBank();
			const int   kind = name == "NewPrefix" ? kPrefixes : kSuffixes;
			size_t      row  = 0;
			bool        found = false;
			for (size_t index = 1; bank != nullptr && !found && AffixRowOf(*bank, kind, index, row); ++index) {
				found = affix::IsBlank(bank->affixes->rows + row * affix::kRow, bank->names) && g_affixEdits.count(AffixKey(kind, index)) == 0;
			}
			g_affixChoosing = false;
			if (!found) {
				view->note = std::string("There is no empty ") + kAffixKinds[kind] + " row left.";
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
			if (at < view->found.size()) {
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
		} else if (cards && name == "Save" && view->card >= 0) {
			if (view->cell >= 0) { // the cell being typed in counts too
				view->pending[view->cell] = view->cellText;
				view->cell                = -1;
			}
			for (const auto& [cell, text] : view->pending) {
				QueueEdit(affixes ? Command::AffixEdit : stats ? Command::StatEdit : Command::Edit, view->card, cell, text);
			}
			view->note = std::to_string(view->pending.size()) + (view->pending.size() == 1 ? " change" : " changes") + " sent to the game.";
			view->pending.clear();
			ask = true;
		} else if (cards && name == "Discard") {
			view->pending.clear();
			view->cell = -1;
			view->note = "Changes discarded.";
		} else if (cards && name == "Restore" && view->card >= 0) {
			view->pending.clear();
			QueueEdit(affixes ? Command::AffixRestore : stats ? Command::StatRestore : Command::Restore, view->card, 0, "");
			view->cell = -1;
			ask        = true;
		} else if (affixes && name == "New" && view->card < 0) {
			g_affixChoosing = true;
			view->note.clear();
		} else if (stats && name == "New" && view->card < 0) {
			QueueEdit(Command::StatNew, 0, 0, "");
			ask = true;
		} else if (cards && name.rfind("Cell", 0) == 0 && view->card >= 0) {
			view->cell     = atoi(name.c_str() + 4);
			view->cell     = view->cell >= 0 && view->cell < (affixes ? affix::kCells : stats ? kStatCells : kCells) ? view->cell : -1;
			view->cellText = CellNow(*view);
		}
	}
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
			const char   c      = *at;
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
				view->pending[view->cell] = view->cellText; // waits for Save changes
				view->note.clear();
				view->cell = c == modbook::kKeyTab && view->cell + 1 < (view == &g_affixView ? affix::kCells : stats ? kNameCell : kCells) ? view->cell + 1 : -1;
				view->cellText = CellNow(*view); // what the next cell holds now
			} else if (static_cast<unsigned char>(c) >= 0x20 && c != modbook::kKeyDelete && text.size() < 48 && c != '"' && c != '\\') {
				text += c;
			}
			if (!inCard) {
				view->top = 0;
				Filter(*view);
			}
		}
	}
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
	const modbook::Tab props { sizeof(modbook::Tab), modbook::kLeft, "properties", "Properties", &g_propsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr };
	const modbook::Tab stats { sizeof(modbook::Tab), modbook::kRight, "stats", "Stats", &g_statsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties" };
	const modbook::Tab funcs { sizeof(modbook::Tab), modbook::kRight, "funcs", "Funcs", &g_funcsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties" };
	g_propsView.handle = book->addTab(&props);
	g_statsView.handle = book->addTab(&stats);
	g_funcsView.handle = book->addTab(&funcs);
	const modbook::Tab affixes { sizeof(modbook::Tab), modbook::kLeft, "affixes", "Affixes", &g_affixView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr };
	g_affixView.handle = book->addTab(&affixes);
	g_book             = book;
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
		if (!commands.empty()) {
			BuildViews();
		}
	}
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
	BookSetUp();
	FlushSay();
	Redraw();
}

// ---------------------------------------------------------------- console

constexpr char kUsage[] = "Live Properties: prop book (Properties) | prop stats | prop funcs | prop affixes - the tabs in the Mod Book (Ctrl+B / Ctrl+L)";

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	if (!g_enabled || !g_link.ok || g_threads == nullptr) {
		g_ctx->WriteConsoleMessage(!g_enabled ? "Live Properties: off in the settings (enabled = false)." : ("Live Properties: off - " + g_linkProblem + ".").c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	char              word[16] = "";
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	const int         got = sscanf(args.c_str(), " %15s", word);
	const std::string w   = got == 1 ? word : "";
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

} // namespace

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
	LogF("Live Properties %s: on - type prop in the console (item service %s, D2R %s)", kVersion, g_items != nullptr && g_inventory != nullptr ? "yes" : "NO: prop give will not work",
		context->buildVersion != nullptr ? context->buildVersion : "?");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	// The tables stay as they are: the memory they point into is never freed.
}
