// Live Properties (proof of concept): item properties of the kind "N% of stat A is added to
// stat B", made, changed and switched off while a game runs. No table reload, no restart.
//
// How: the game's stat table already has unused rows (Exalted Items fills the numbers between
// the mod's last stat and its own with empty rows). A live property takes one of them and is
// wired by hand the way the game wires an "op 2" stat after reading the table (prop_core.h).
// Each property also gets a row at the end of the game's property table, so the game's own
// property code (cube recipes, item creation) can put it on an item.
//
// Console (Ctrl + `):
//   prop                               the properties and their numbers
//   prop set <1-8> <stat A> <stat B>   property N becomes "N% of A is added to B" (names or numbers)
//   prop text <1-8> <words>            the line an item shows after "+N% " (default: "of A added to B")
//   prop off <1-8>                     the property stops doing anything (items keep the stat)
//   prop give <1-8> <percent> [code]   a test item with the property (default: small charm cm1)
//   prop book / prop stats             the Mod Book tabs: every property of the game (left), every stat (right)
//
// The Properties tab shows every row of the game's property table; a row's card lets its
// func / stat / set / val cells be changed. The plugin's own copy of the table is what the game
// reads, so a change counts at once for items made from then on. Changed rows are kept in
// d2rloader\live-properties\<mod>\property-edits.txt and put back when the tables are read.
//
// The definitions are kept in d2rloader\live-properties\<mod>\properties.txt and put back
// every time the game has read its tables. The mod's own files are never written.
//
// The item's line of text is one of eight strings the plugin gives the game at start; its
// letters are written over in the game's memory when a property changes.
// A change is followed by touching the property's stat on every item the character wears or
// carries, which makes the game work the affected stats out again.
//
// What this build does NOT do yet: a property code usable in recipes.txt, checking saved
// items before a change.

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
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.4.2";
constexpr int         kSlots   = 8;
constexpr int         kSlotTop = 486; // property 1 is stat 486, property 2 is 485 ...

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
	{ 0x2fd1dc, "the property table's unload check", false, 7, { 0x4c, 0x39, 0xb7, 0x50, 0x02, 0x00, 0x00 } },
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

// <loader folder>\live-properties\<mod>: beside the plugins folder. Never inside a mod's .mpq.
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
	return loader + L"\\live-properties\\" + Widen(mod);
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

struct Def { // one property, as the user typed it
	bool        on = false;
	std::string base, target;
	std::string text; // the item's line after "+N% "; empty = "of <base> added to <target>"
};

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
};

SRWLOCK                  g_lock = SRWLOCK_INIT; // everything below
Def                      g_defs[kSlots];
Bank                     g_banks[3];
bool                     g_setUp = false;
std::wstring             g_folder;
std::vector<std::string> g_say;

struct Command {
	enum Kind { List, Set, Off, Give, Text, Edit, Restore, StatEdit, StatRestore, StatNew } kind = List; // Edit: slot = property row, percent = cell, a = text
	int                                slot = 0, percent = 0;
	std::string                        a, b;
	D2RL::PlayerHandle                 player = D2RL::InvalidPlayerHandle;
};
std::vector<Command> g_commands;

// The eight strings (text.json below). kTextRoom letters each: the game keeps them as they
// were read, and the plugin writes the real text over them.
constexpr int      kTextRoom = 90;
constexpr uint16_t kPriority = 60;
uint16_t           g_unknownId = 0xffff; // what the game answers for a string key it does not have
uint16_t           g_stringIds[kSlots] = {};
char*              g_strings[kSlots]   = {}; // the game's own copies; nullptr = not found
std::string        g_stringsJson;

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

uint16_t SlotStat(int slot) {
	return static_cast<uint16_t>(kSlotTop + 1 - slot);
}

std::wstring DefsPath() {
	return g_folder + L"\\properties.txt";
}

void ReadDefs() {
	for (Def& def : g_defs) {
		def = Def {};
	}
	std::string text;
	if (!ReadWhole(DefsPath(), text)) {
		return;
	}
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at);
		end        = end == std::string::npos ? text.size() : end;
		char a[64] = "", b[64] = "", line[128] = "";
		int  slot  = 0;
		if (sscanf(text.substr(at, end - at).c_str(), " %d %63s %63s %127[^\r\n]", &slot, a, b, line) >= 3 && slot >= 1 && slot <= kSlots) {
			g_defs[slot - 1] = { true, a, b, line };
		}
		at = end + 1;
	}
}

void WriteDefs() {
	std::string text = "# Live Properties: property number, the stat a percentage is taken of, the stat it is added to, the item's text.\r\n"
	                   "# Written by the plugin (console: prop set / prop off). Read when the game has read its tables.\r\n";
	for (int i = 0; i < kSlots; ++i) {
		if (g_defs[i].on) {
			text += std::to_string(i + 1) + "\t" + g_defs[i].base + "\t" + g_defs[i].target + (g_defs[i].text.empty() ? "" : "\t" + g_defs[i].text) + "\r\n";
		}
	}
	MakeFolders(g_folder);
	if (!WriteOwn(DefsPath(), text)) {
		Say("could not write %s - the change holds until the game closes", Narrow(DefsPath()).c_str());
	}
}

// A stat's number from what the user typed: a number, or a name the game's own table knows.
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

void LogRow(const Bank& bank, uint16_t stat) {
	const uint8_t* row = bank.stats->rows + stat * prop::kStatRow;
	char           text[3 * 0x60 + 1];
	for (int i = 0; i < 0x60; ++i) {
		snprintf(text + i * 3, 4, "%02x ", row[i]);
	}
	LogF("data version %d, stat %d, first bytes: %s", bank.version, stat, text);
}

// ---------------------------------------------------------------- the item's line of text

std::string Placeholder(int slot) {
	std::string text = "live property " + std::to_string(slot) + " ";
	text.resize(kTextRoom, '.');
	return text;
}

// The strings file handed to the game at plugin start: keys Prop1..Prop8 in the plugin's own
// namespace, the same text for every language (a missing language shows nothing).
void RegisterStrings(const D2RL::ResourceService* resources) {
	static const char* const kLanguages[] = { "enUS", "zhTW", "deDE", "esES", "frFR", "itIT", "koKR", "plPL", "esMX", "jaJP", "ptBR", "ruRU", "zhCN" };
	g_stringsJson = "[\n";
	for (int slot = 1; slot <= kSlots; ++slot) {
		g_stringsJson += "  { \"Key\": \"Prop" + std::to_string(slot) + "\"";
		for (const char* language : kLanguages) {
			g_stringsJson += std::string(", \"") + language + "\": \"" + Placeholder(slot) + "\"";
		}
		g_stringsJson += slot < kSlots ? " },\n" : " }\n";
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
	for (int slot = 1; slot <= kSlots; ++slot) {
		const std::string key = "live-properties:Prop" + std::to_string(slot);
		const View        view { key.data(), key.size() };
		const uint16_t    id  = g_link.stringId(&view);
		char*             at  = id != unknown ? const_cast<char*>(g_link.stringById(id)) : nullptr;
		const bool        was = g_strings[slot - 1] != nullptr && g_strings[slot - 1] == at; // ours from before a table reload
		if (at == nullptr || !Readable(at, kTextRoom + 1) || (!was && memcmp(at, Placeholder(slot).c_str(), kTextRoom + 1) != 0)) {
			LogF("text: string %s was not found as the plugin gave it (id %u, unknown = %u); property %d will show no line", key.c_str(), id, unknown, slot);
			g_strings[slot - 1]   = nullptr;
			g_stringIds[slot - 1] = 0;
			continue;
		}
		g_strings[slot - 1]   = at;
		g_stringIds[slot - 1] = id;
	}
}

void WriteText(int slot) {
	char* at = g_strings[slot - 1];
	if (at == nullptr) {
		return;
	}
	const Def&  def  = g_defs[slot - 1];
	std::string text = !def.on ? "(live property " + std::to_string(slot) + ", switched off)" : !def.text.empty() ? def.text : "of " + def.base + " added to " + def.target;
	text.resize(text.size() < static_cast<size_t>(kTextRoom) ? text.size() : kTextRoom);
	memcpy(at, text.c_str(), text.size() + 1);
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

// Gives the property table its extra rows: a copy of the game's table with one row per
// property at the end, and the game's vector pointed at the copy.
bool GrowProperties(Bank& bank) {
	Vector* v = bank.props;
	if (bank.propRows != nullptr && v->rows == bank.propRows) {
		return true; // still ours
	}
	if (!Readable(v->rows, v->count * prop::kPropRow) || v->count == 0 || v->count > 0x7000) {
		bank.off = "its property table cannot be read";
		return false;
	}
	// rows hold their own number first; an unused stat cell is 0xffff
	for (uint16_t n = 0; n < 4 && n < v->count; ++n) {
		uint16_t first;
		memcpy(&first, v->rows + n * prop::kPropRow, 2);
		if (first != n) {
			bank.off = "its property rows do not start with their number";
			return false;
		}
	}
	const uint64_t base = v->count;
	auto*          rows = static_cast<uint8_t*>(VirtualAlloc(nullptr, (base + kSlots) * prop::kPropRow, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // never freed: the game may hold it
	if (rows == nullptr) {
		bank.off = "no memory for the property table";
		return false;
	}
	memcpy(rows, v->rows, base * prop::kPropRow);
	bank.propOrig.assign(rows, rows + base * prop::kPropRow);
	for (int i = 0; i < kSlots; ++i) {
		prop::FillProperty(rows + (base + i) * prop::kPropRow, static_cast<uint16_t>(base + i), SlotStat(i + 1));
	}
	v->rows       = rows; // same rows as before, so any order of these three writes is safe
	v->capacity   = (base + kSlots) | kNotOwned;
	v->count      = base + kSlots;
	bank.propRows = rows;
	bank.propBase = base;
	return true;
}

bool Apply(Bank& bank, int slot, std::string& problem) {
	const Def&     def = g_defs[slot - 1];
	const uint16_t s   = SlotStat(slot);
	if (!def.on) {
		prop::Unwire(bank.stats->rows, bank.stats->count, s);
		return true;
	}
	uint16_t a = 0, t = 0;
	if (!StatNumber(bank, def.base, a)) {
		problem = "there is no stat named '" + def.base + "' (use a name from itemstatcost.txt, or its number)";
		return false;
	}
	if (!StatNumber(bank, def.target, t)) {
		problem = "there is no stat named '" + def.target + "' (use a name from itemstatcost.txt, or its number)";
		return false;
	}
	const auto live = [](uint16_t stat) { return stat >= kSlotTop + 1 - kSlots && stat <= kSlotTop; };
	if (live(a) || live(t)) {
		problem = "neither stat can be a live property's own";
		return false;
	}
	if (!prop::Wire(bank.stats->rows, bank.stats->count, s, a, t, problem)) {
		LogRow(bank, s);
		return false;
	}
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

bool IsLiveSlot(uint16_t stat) {
	return stat > kSlotTop - kSlots && stat <= kSlotTop;
}

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

// Is this string number one the table already shows (or one of the plugin's eight)?
bool StringInUse(const Bank& bank, uint16_t id) {
	for (const uint16_t own : g_stringIds) {
		if (own != 0 && own == id) {
			return true;
		}
	}
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
			value = id;
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
//  * the 8 live property rows are not touched here;
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
	if (IsLiveSlot(stat)) {
		problem = "this row is a live property's own (console: prop set / prop text / prop off)";
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

// Looks at the tables as they are now and puts every saved property back. Game thread, or the
// tables-read notice.
void SetUp() {
	g_setUp  = true;
	g_folder = OwnFolder();
	ReadDefs();
	ReadEdits();
	ReadStatEdits();
	if (!CheckPrints()) {
		return;
	}
	FindStrings();
	for (int slot = 1; slot <= kSlots; ++slot) {
		WriteText(slot);
	}
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
		bank.props = reinterpret_cast<Vector*>(tables + kPropsAt);
		if (bank.stats->count <= static_cast<uint64_t>(kSlotTop) || bank.stats->count > 0x8000 || !Readable(bank.stats->rows, bank.stats->count * prop::kStatRow)) {
			bank.off = "its stat table has no rows up to " + std::to_string(kSlotTop) + " (" + std::to_string(bank.stats->count) + " rows)";
			continue;
		}
		// the rows as the game read them: taken once per table (a row of ours in it = taken before)
		if (bank.statOrigOf != bank.stats->rows || bank.statOrig.size() != bank.stats->count * prop::kStatRow || prop::IsBlank(bank.stats->rows + SlotStat(1) * prop::kStatRow)) {
			bank.statOrig.assign(bank.stats->rows, bank.stats->rows + bank.stats->count * prop::kStatRow);
			bank.statOrigOf = bank.stats->rows;
		}
		int seen = 0;
		if (!LayoutHolds(bank, seen)) {
			bank.off = "its stat table is not linked the way the plugin expects";
			continue;
		}
		bool free = true;
		for (int slot = 1; slot <= kSlots && free; ++slot) {
			const uint8_t* row = bank.stats->rows + SlotStat(slot) * prop::kStatRow;
			if (!prop::IsBlank(row) && !prop::IsOurs(row)) {
				bank.off = "stat " + std::to_string(SlotStat(slot)) + " is in use";
				LogRow(bank, SlotStat(slot));
				free = false;
			}
		}
		if (!free || !GrowProperties(bank)) {
			continue;
		}
		bank.on = true;
		WalkNames(tables, kStatNamesAt, bank.stats->count, bank.statNames);
		WalkNames(tables, kPropNamesAt, bank.propBase, bank.propNames);
		ApplyEdits(bank);
		ApplyStatEdits(bank);
		int applied = 0, lines = 0;
		for (int slot = 1; slot <= kSlots; ++slot) { // the save shape and the line of text, for every property, set or not
			if (g_strings[slot - 1] != nullptr && prop::Shape(bank.stats->rows + SlotStat(slot) * prop::kStatRow, g_stringIds[slot - 1], kPriority)) {
				++lines;
			}
		}
		if (lines != 0 && g_link.descOrder != nullptr) {
			g_link.descOrder(bank.tables); // the game's list of stats that have a line, in the order they are shown
		}
		for (int slot = 1; slot <= kSlots; ++slot) {
			std::string problem;
			if (!Apply(bank, slot, problem)) {
				Say("data version %d: property %d (%s -> %s) was not set up: %s", bank.version, slot, g_defs[slot - 1].base.c_str(), g_defs[slot - 1].target.c_str(), problem.c_str());
			} else if (g_defs[slot - 1].on) {
				++applied;
			}
		}
		LogF("data version %d: on - %d op stats checked, stats %d..%d are the properties, property numbers %llu..%llu, %d set up", bank.version, seen, kSlotTop,
			kSlotTop + 1 - kSlots, static_cast<unsigned long long>(bank.propBase), static_cast<unsigned long long>(bank.propBase + kSlots - 1), applied);
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
	if (command.kind == Command::List) {
		for (int slot = 1; slot <= kSlots; ++slot) {
			const Def& def = g_defs[slot - 1];
			uint16_t   a = 0, t = 0;
			const bool wired = prop::IsWired(main->stats->rows + SlotStat(slot) * prop::kStatRow, &a, &t);
			if (def.on) {
				Say("%d: N%% of %s is added to %s  (stat %d, property number %llu%s)  text: +N%% %s", slot, def.base.c_str(), def.target.c_str(), SlotStat(slot),
					static_cast<unsigned long long>(main->propBase + slot - 1), wired ? "" : "; NOT set up, see the log",
					g_strings[slot - 1] != nullptr ? g_strings[slot - 1] : "(no line: string not found)");
			} else {
				Say("%d: free", slot);
			}
		}
		return;
	}
	if (command.kind == Command::Text) {
		if (!g_defs[command.slot - 1].on) {
			Say("property %d is free; set it first", command.slot);
			return;
		}
		g_defs[command.slot - 1].text = command.a;
		WriteText(command.slot);
		WriteDefs();
		Say("property %d's line is now: +N%% %s%s", command.slot, g_strings[command.slot - 1] != nullptr ? g_strings[command.slot - 1] : command.a.c_str(),
			g_strings[command.slot - 1] != nullptr ? "" : "  (not shown: the game's string was not found, see the log)");
		return;
	}
	if (command.kind == Command::Set || command.kind == Command::Off) {
		const Def before         = g_defs[command.slot - 1];
		g_defs[command.slot - 1] = command.kind == Command::Set ? Def { true, command.a, command.b, before.on && before.base == command.a && before.target == command.b ? before.text : "" } : Def {};
		bool        ok           = true;
		std::string problem;
		// First the old links go while the row still names its old target: touching the stat
		// now makes the game work that target out again, without the property.
		for (Bank& bank : g_banks) {
			if (bank.on) {
				prop::Unlink(bank.stats->rows, bank.stats->count, SlotStat(command.slot));
			}
		}
		int touched = Recalculate(command.player, SlotStat(command.slot));
		for (Bank& bank : g_banks) {
			if (bank.on && !Apply(bank, command.slot, problem)) {
				ok = false;
				Say("data version %d: %s", bank.version, problem.c_str());
			}
		}
		if (!ok) { // back to what it was, everywhere
			g_defs[command.slot - 1] = before;
			for (Bank& bank : g_banks) {
				if (bank.on) {
					Apply(bank, command.slot, problem);
				}
			}
			static_cast<void>(Recalculate(command.player, SlotStat(command.slot)));
			Say("property %d is unchanged", command.slot);
			return;
		}
		if (command.kind == Command::Set) {
			touched = Recalculate(command.player, SlotStat(command.slot)); // and the new target, with the property
		}
		WriteText(command.slot);
		WriteDefs();
		char after[160];
		if (touched < 0) {
			snprintf(after, sizeof(after), "The game was NOT made to recalculate (not possible here): move or re-equip items that have it.");
		} else {
			snprintf(after, sizeof(after), "Recalculated on %d item%s you wear or carry.", touched, touched == 1 ? "" : "s");
		}
		if (command.kind == Command::Set) {
			Say("property %d is now: N%% of %s is added to %s. %s", command.slot, command.a.c_str(), command.b.c_str(), after);
		} else {
			Say("property %d is off. Items keep the stat, and it does nothing. %s", command.slot, after);
		}
		return;
	}
	if (command.kind == Command::Edit || command.kind == Command::Restore) {
		RunEdit(command);
		return;
	}
	if (command.kind == Command::StatEdit || command.kind == Command::StatRestore || command.kind == Command::StatNew) {
		RunStat(command);
		return;
	}
	// Give
	if (g_items == nullptr || command.player == D2RL::InvalidPlayerHandle) {
		Say("no test item: %s", g_items == nullptr ? "this D2RLoader has no item service" : "no character is in a game");
		return;
	}
	if (!g_defs[command.slot - 1].on) {
		Say("property %d is free; set it first (prop set %d <stat A> <stat B>)", command.slot, command.slot);
		return;
	}
	char code[5] = "    ";
	memcpy(code, command.a.data(), command.a.size() < 4 ? command.a.size() : 4);
	const D2RL::Items::PropertySpec   property = D2RL::Items::MakeExactProperty(static_cast<uint32_t>(main->propBase + command.slot - 1), command.percent);
	const D2RL::Items::ItemCreateSpec spec {
		.structSize      = D2RL::Items::ItemCreateSpecSize,
		.code            = D2RL::Items::MakeItemCode(code[0], code[1], code[2], code[3]),
		.quality         = D2RL::Items::Quality::Magic,
		.qualityRecordId = D2RL::Items::RandomQualityRecord,
		.itemLevel       = 1,
		.seedMode        = D2RL::Items::SeedMode::Random,
		.quantity        = D2RL::Items::DefaultValue,
		.durability      = D2RL::Items::DefaultValue,
		.stateFlags      = D2RL::Items::ItemStateIdentified,
		.propertyCount   = 1,
		.properties      = &property,
		.destination     = {
			.structSize = D2RL::Items::ItemDestinationSize,
			.container  = D2RL::Items::ItemContainer::Inventory,
			.placement  = D2RL::Items::Placement::Automatic,
		},
	};
	D2RL::ItemHandle item   = D2RL::InvalidItemHandle;
	const auto       result = g_items->createItem(g_ctx, command.player, &spec, &item);
	if (result != D2RL::Items::Result::Success) {
		Say("the game did not make the item '%s' (result %u; 12 = not the host, 3 = no game, 4 = no room)", command.a.c_str(), static_cast<unsigned>(result));
		return;
	}
	Say("made a '%s' with property %d at %d%% (stat %d) in your inventory", command.a.c_str(), command.slot, command.percent, SlotStat(command.slot));
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
};
ListView g_statsView, g_propsView, g_funcsView;
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
			const int slot    = kSlotTop + 1 - static_cast<int>(i);
			if (slot >= 1 && slot <= kSlots) {
				const Def& def = g_defs[slot - 1];
				entry.detail   = "live property " + std::to_string(slot) + (def.on ? ": N% of " + def.base + " is added to " + def.target : ": free (console: prop set " + std::to_string(slot) + " <stat A> <stat B>)");
			} else if (prop::IsBlank(row)) {
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
			if (i >= bank->propBase) {
				const int  slot = static_cast<int>(i - bank->propBase) + 1;
				const Def& def  = g_defs[slot - 1];
				entry.title     = std::to_string(i) + "  live property " + std::to_string(slot);
				entry.detail    = def.on ? "N% of " + def.base + " is added to " + def.target + " (console: prop set / prop text / prop off)" : "free (console: prop set " + std::to_string(slot) + " <stat A> <stat B>)";
			} else {
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
	for (ListView* view : { &g_statsView, &g_propsView, &g_funcsView }) {
		Filter(*view);
		if (view->card >= static_cast<int>(view->entries.size())) {
			view->card = view->cell = -1;
		}
	}
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
		for (int n = kSlotTop - kSlots; n >= 0 && free < 0; --n) {
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
	const std::string key  = own ? PropKey(*bank, row) : "live property " + std::to_string(row - bank->propBase + 1);
	const Cells       cells = RowCells(*bank, bank->propRows + row * prop::kPropRow);
	g_book->title(page, ("Property " + std::to_string(row) + ": " + key).c_str());
	if (own && g_edits.count(key) != 0) {
		g_book->button(page, "Restore", 742, 172, 330, "The game's own row", "$FontColorLightRed");
	}
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
			const std::string text   = typing ? v.cellText + "_" : cells[static_cast<size_t>(cell)].empty() ? "-" : cells[static_cast<size_t>(cell)];
			g_book->label(page, ("Text" + name).c_str(), xs[c], y, widths[c], 36, typing ? "$FontColorWhite" : "$FontColorLightGold", "left", 0, Cut(text, c == 1 ? 28 : 9).c_str());
			if (own) {
				g_book->spot(page, ("Spot" + name).c_str(), xs[c], y, widths[c] - 10, 40, name.c_str(), tips[c]);
			}
		}
		const int func = atoi(cells[static_cast<size_t>(line * 4)].c_str());
		g_book->label(page, ("Words" + n).c_str(), 130, y + 40, 940, 36, "$FontColorGoldYellow", "left", 0, func != 0 ? (std::string("func ") + std::to_string(func) + ": " + FuncWords(func)).c_str() : "");
	}
	g_book->label(page, "Note", 82, 1000, 990, 100, "$FontColorGreen", "left", 1, v.note.c_str());
	g_book->label(page, "Help", 82, 1110, 990, 180, "$FontColorGoldYellow", "left", 1,
		own ? "Click a cell, type, Enter sets it (Tab: sets it and goes to the next cell, Esc: leaves it). With a func or stat cell open, a click in the Funcs or Stats tab (Ctrl+L) puts it in. "
		      "A change counts at once for items made from now on; items that exist keep the stats they have."
		    : "This row is a live property's own. Change it in the console: prop set / prop text / prop off.");
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
	const bool      live   = IsLiveSlot(stat);
	const auto      edited = g_statEdits.find(stat);
	const bool      named  = !GameStatName(*bank, stat).empty() && GameStatName(*bank, stat).rfind("xlt_unused_", 0) != 0;
	StatCells       cells  = StatRowCells(*bank, row);
	cells[kNameCell]       = edited != g_statEdits.end() ? edited->second[kNameCell] : "";
	g_book->title(page, ("Stat " + std::to_string(stat) + ": " + StatWord(*bank, stat)).c_str());
	if (edited != g_statEdits.end()) {
		g_book->button(page, "Restore", 742, 172, 330, named ? "The game's own row" : "Remove this stat", "$FontColorLightRed");
	}
	static const char* const groups[] = { "saved", "text", "op" };
	for (int i = 0; i < kStatCells; ++i) {
		if (i == kNameCell && named) {
			continue;
		}
		const int         y      = 268 + i * 54;
		const std::string n      = std::to_string(i);
		const bool        typing = i == v.cell;
		const std::string text   = typing ? v.cellText + "_" : cells[static_cast<size_t>(i)].empty() ? "-" : cells[static_cast<size_t>(i)];
		std::string       hint   = i == kNameCell ? "your name for it (used in the lists and in stat cells)" : groups[kFields[i].group];
		if (i < kNameCell && kFields[i].kind == kCellString) {
			const std::string words = StringText(static_cast<uint16_t>(FieldValue(row, kFields[i])));
			hint                    = words.empty() ? hint : "\"" + words + "\"";
		}
		g_book->label(page, ("Name" + n).c_str(), 82, y, 300, 36, "$FontColorGoldYellow", "left", 0, i == kNameCell ? "name" : kFields[i].name);
		g_book->label(page, ("Text" + n).c_str(), 390, y, 330, 36, typing ? "$FontColorWhite" : "$FontColorLightGold", "left", 0, Cut(text, 19).c_str());
		g_book->label(page, ("Hint" + n).c_str(), 730, y, 340, 36, "$FontColorGoldYellow", "left", 0, Cut(hint, 20).c_str());
		if (!live) {
			g_book->spot(page, ("Spot" + n).c_str(), 82, y, 980, 50, ("Cell" + n).c_str(),
				i == kNameCell ? "The plugin's own name for a stat you made. The game does not know it; this plugin's lists and cells do." : kFields[i].tip);
		}
	}
	g_book->label(page, "Note", 82, 268 + kStatCells * 54 + 8, 990, 130, live ? "$FontColorGoldYellow" : "$FontColorGreen", "left", 1,
		live ? "This row is a live property's own. Change it in the console: prop set / prop text / prop off."
		     : v.note.empty() ? "Click a line, type, Enter sets it (Tab: next line, Esc: leaves it). Hover a line for what it means. 'saved' lines are refused while you carry an item with the stat." : v.note.c_str());
}

void __cdecl TabDraw(void* user, modbook::Page* page) noexcept {
	Guard guard;
	if (user == &g_funcsView) {
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

// What a card's cell holds now. Under the lock.
std::string CellNow(const ListView& view) {
	const Bank* bank = MainBank();
	if (bank == nullptr || view.card < 0 || view.cell < 0) {
		return {};
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
	const bool stats = view == &g_statsView, cards = view != &g_funcsView;
	{
		Guard             guard;
		const std::string name = what != nullptr ? what : "";
		if (name == "Escape") {
			if (cards && view->cell >= 0) {
				view->cell = -1;
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
				if (view == &g_propsView) {
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
		} else if (cards && name == "Back") {
			view->card = view->cell = -1;
		} else if (cards && name == "Restore" && view->card >= 0) {
			QueueEdit(stats ? Command::StatRestore : Command::Restore, view->card, 0, "");
			view->cell = -1;
			ask        = true;
		} else if (stats && name == "New" && view->card < 0) {
			QueueEdit(Command::StatNew, 0, 0, "");
			ask = true;
		} else if (cards && name.rfind("Cell", 0) == 0 && view->card >= 0) {
			view->cell     = atoi(name.c_str() + 4);
			view->cell     = view->cell >= 0 && view->cell < (stats ? kStatCells : kCells) ? view->cell : -1;
			view->cellText = CellNow(*view);
		}
	}
	if (ask) {
		Ask();
	}
	if (close && g_book != nullptr) {
		g_book->close(view->side);
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
				QueueEdit(stats ? Command::StatEdit : Command::Edit, view->card, view->cell, view->cellText);
				ask        = true;
				view->cell = c == modbook::kKeyTab && view->cell + 1 < (stats ? kNameCell : kCells) ? view->cell + 1 : -1;
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
int __cdecl  TabLeave(void*) noexcept {
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
	g_propsView.side = modbook::kLeft;
	const modbook::Tab props { sizeof(modbook::Tab), modbook::kLeft, "properties", "Properties", &g_propsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr };
	const modbook::Tab stats { sizeof(modbook::Tab), modbook::kRight, "stats", "Stats", &g_statsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties" };
	const modbook::Tab funcs { sizeof(modbook::Tab), modbook::kRight, "funcs", "Funcs", &g_funcsView, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, "Properties" };
	g_propsView.handle = book->addTab(&props);
	g_statsView.handle = book->addTab(&stats);
	g_funcsView.handle = book->addTab(&funcs);
	g_book             = book;
	LogF("Mod Book: tabs Properties (left, %s), Stats and Funcs (right, %s, %s)", g_propsView.handle != 0 ? "added" : "REFUSED, see mod-book.log", g_statsView.handle != 0 ? "added" : "REFUSED",
		g_funcsView.handle != 0 ? "added" : "REFUSED");
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

constexpr char kUsage[] = "Live Properties: prop | prop book | prop stats | prop funcs | prop set <1-8> <stat A> <stat B> | prop text <1-8> <words> | prop off <1-8> | prop give <1-8> <percent 1-500> [item code]";

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	if (!g_enabled || !g_link.ok || g_threads == nullptr) {
		g_ctx->WriteConsoleMessage(!g_enabled ? "Live Properties: off in the settings (enabled = false)." : ("Live Properties: off - " + g_linkProblem + ".").c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	char word[16] = "", a[64] = "", b[64] = "", c[64] = "";
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	const int         got = sscanf(args.c_str(), " %15s %63s %63s %63s", word, a, b, c);
	Command           command;
	const int         slot = atoi(a);
	if (got == 1 && (strcmp(word, "book") == 0 || strcmp(word, "stats") == 0 || strcmp(word, "funcs") == 0)) {
		BookSetUp();
		if (g_book == nullptr) {
			g_ctx->WriteConsoleMessage("Live Properties: the Mod Book plugin (d2rl-mod-book.dll) is not installed; there are no panels without it.");
		} else {
			g_book->toggle(word[0] == 'b' ? g_propsView.handle : word[0] == 'f' ? g_funcsView.handle : g_statsView.handle);
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (got <= 0 || strcmp(word, "list") == 0) {
		command.kind = Command::List;
	} else if (strcmp(word, "set") == 0 && got == 4 && slot >= 1 && slot <= kSlots) {
		command = { Command::Set, slot, 0, b, c };
	} else if (strcmp(word, "text") == 0 && got >= 3 && slot >= 1 && slot <= kSlots) {
		const size_t at = args.find(a, args.find("text") + 4) + strlen(a);
		std::string  line = args.substr(args.find_first_not_of(" \t", at));
		for (char& letter : line) {
			letter = letter == '\t' || letter == '"' || letter == '\\' ? ' ' : letter;
		}
		command = { Command::Text, slot, 0, line };
	} else if (strcmp(word, "off") == 0 && got == 2 && slot >= 1 && slot <= kSlots) {
		command = { Command::Off, slot };
	} else if (strcmp(word, "give") == 0 && got >= 3 && slot >= 1 && slot <= kSlots && atoi(b) >= 1 && atoi(b) <= prop::kMaxPct) {
		command = { Command::Give, slot, atoi(b), got == 4 ? c : "cm1" };
	} else {
		g_ctx->WriteConsoleMessage(kUsage);
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (g_inventory != nullptr) {
		static_cast<void>(g_inventory->getLocalPlayer(g_ctx, &command.player));
	}
	{
		Guard guard;
		g_commands.push_back(command);
	}
	Ask();
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
