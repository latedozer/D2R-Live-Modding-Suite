// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Cube Recipes: your own Horadric Cube recipes, taken in while the game runs.
//
// The recipes are lines in a file of the plugin's own, d2rloader\live-cube-recipes\<mod>\recipes.txt
// (cubemain.txt columns). Add, change or delete a line, save, type "recipes" in the console, and
// the cube uses it: no restart, no Save and Quit. The mod's files are only ever read, never written.
//
// How (D2R image stamp 0x6ab3782c under D2RLoader 1.3):
//  * The game keeps its recipes as a list of 0x16c-byte records per data set, in a vector at
//    data tables + 0x1570 {records, count, capacity}; the cube walks it through 0x3e3f20 (count)
//    and 0x3e3f60 (record n) on every transmute (0x529090) and takes the first recipe that fits.
//  * The file is read at two moments only: when the game has read its tables, and when the
//    console command "recipes" is typed. Nothing watches it in between. Its text is handed to
//    the game's own thread, where every line is made into a record: the input and
//    output cells by the game's own parsers (D2RCore's CompileCubeInputField and
//    CompileCubeOutputField, the functions the game's table loader calls), property codes by the
//    game's own column function (0x3203e0), numbers as plain numbers (cube_core.cpp). Because the
//    parsers look names up in the tables the running game has loaded, every item, item type,
//    unique, set item and property of the mod and of other plugins can be used.
//  * The user's recipes are laid over a copy of the game's list (cube::Overlay): a line with the
//    ingredients of a recipe of the game stands in its place, any other line is a new recipe in
//    front. The result is written into memory of the plugin's own and the vector is pointed at
//    it, with the "not the vector's memory" bit set in the capacity, so the game never frees it.
//    The game's own list is never written to.
//  * When the game reads its tables (start, D2RLoader's reload) the file is laid over the new list.
//  * After each of the two moments the cube's whole list is written out as text to
//    all-recipes.txt beside the user's file. A record holds no text, so each record's text is
//    the line of a recipe file (the user's, the mod's cubemain.txt, other plugins' copies of it
//    in the loader folders) that compiles to exactly that record (cube::ListAll).

#include <D2RLPlugin/api.h>

#include "cube_core.h"
#include "../Mod Book/mod_book_api.h"
#include "../Live Properties Mod/live_props_api.h"
#include "../Live Tables/live_tables_api.h"

#include <windows.h>
#include <bcrypt.h>


#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

#define LCR_VERSION "0.16.7"
constexpr const char* kVersion = LCR_VERSION;
// The version in the DLL's export table, for install.sh (Suite .dlls/previous/<name>.<version>.dll).
extern "C" __declspec(dllexport) const char LiveSuiteVersion[] = "d2rl-live-cube-recipes " LCR_VERSION;

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

uint64_t NowMs() {
	return GetTickCount64();
}

double NowUs() {
	static LARGE_INTEGER frequency {};
	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	return static_cast<double>(counter.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
}

// ---------------------------------------------------------------- settings

struct Config {
	bool enabled = true;
	bool console = true;
};
Config g_config;

void LoadConfig() {
	static char text[8192];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		LogF("settings: could not read live-cube-recipes.toml - using the defaults");
		return;
	}
	for (const char* line = text; line != nullptr && *line != '\0';) {
		const char* end = strchr(line, '\n');
		char        buffer[256], key[32], value[32];
		snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(end != nullptr ? end - line : static_cast<ptrdiff_t>(strlen(line))), line);
		line = end != nullptr ? end + 1 : nullptr;
		if (sscanf(buffer, " %31[a-z_] = %31s", key, value) != 2) {
			continue; // comments, [sections] and blank lines
		}
		const bool on = strncmp(value, "true", 4) == 0 || *value == '1';
		if (strcmp(key, "enabled") == 0) {
			g_config.enabled = on;
		} else if (strcmp(key, "console") == 0) {
			g_config.console = on;
		}
	}
}

// ---------------------------------------------------------------- the game

constexpr uint32_t kKnownStamp = 0x6ab3782c; // the D2R image all addresses below are from
constexpr uint32_t kTestStamp  = 0x7e577e57; // test/fake_host.cpp: functions are listed at +0x2000

constexpr uintptr_t kTablesRva = 0x300a90; // data tables of a data version (1 classic, 2 LoD, 3 RotW)
constexpr uintptr_t kModRva    = 0x3203e0; // the "mod N" column function: property code -> number
constexpr uintptr_t kParamRva  = 0x3e5da0; // the "param" column function: a name -> number
constexpr uintptr_t kLookupRva = 0xa121f0; // code -> row number in one of the game's name tables
constexpr uintptr_t kVectorAt  = 0x1570;   // the recipe vector in the data tables
constexpr uintptr_t kClassesAt = 0x18;     // the class code name table in the data tables

// The recipe vector. A capacity with the top bit set means the memory is not the vector's:
// the game then neither frees nor grows it (0x2fc75b, 0x3e3e60, 0x3e5253).
struct Vector {
	uint8_t* rows;
	uint64_t count;
	uint64_t capacity;
};
constexpr uint64_t kNotOwned = 0x8000000000000000ull;

struct Link {
	bool ok = false;
	void* (*tables)(uint8_t version)                                                   = nullptr;
	void (*mod)(uint8_t version, const char* text, void* record, int offset)          = nullptr;
	void (*param)(uint8_t version, const char* text, uint32_t* value)                 = nullptr;
	int (*lookup)(void* table, const char* text, int unused)                          = nullptr;
	// D2RCore's exports: (version, cell text, record, slot, unused, line); 0 = not understood
	int (*input)(uint8_t version, char* text, void* record, int slot, uint64_t unused, int line)  = nullptr;
	int (*output)(uint8_t version, char* text, void* record, int slot, uint64_t unused, int line) = nullptr;
};
Link        g_link;
std::string g_linkProblem;

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
	{ kModRva, "the property column function", true, 16, { 0x4d, 0x85, 0xc0, 0x0f, 0x84, 0x07, 0x01, 0x00, 0x00, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x20 } },
	{ kParamRva, "the param column function", true, 16, { 0x4d, 0x85, 0xc0, 0x74, 0x6b, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x49 } },
	{ kLookupRva, "the name table lookup", true, 16, { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x05 } },
	// the cube's own reader: recipe vector at +0x1570, 0x16c bytes a recipe
	{ 0x3e3f9a, "the recipe list's place", false, 7, { 0x48, 0x8d, 0x9e, 0x70, 0x15, 0x00, 0x00 } },
	{ 0x3e3fca, "the recipe size", false, 9, { 0x48, 0x69, 0x44, 0x24, 0x50, 0x6c, 0x01, 0x00, 0x00 } },
};

bool      g_printsChecked = false;
uintptr_t g_imageBase     = 0;

// At plugin start: is this the game build the addresses are from, and does D2RCore offer the
// two cell compilers?
bool LinkGame() {
	g_imageBase = g_ctx->exeBase != 0 ? g_ctx->exeBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const uintptr_t base  = g_imageBase;
	const uint32_t  stamp = ImageStamp(base);
	if (stamp == kTestStamp) {
		void** list     = reinterpret_cast<void**>(base + 0x2000);
		g_link.tables   = reinterpret_cast<decltype(g_link.tables)>(list[0]);
		g_link.mod      = reinterpret_cast<decltype(g_link.mod)>(list[1]);
		g_link.param    = reinterpret_cast<decltype(g_link.param)>(list[2]);
		g_link.lookup   = reinterpret_cast<decltype(g_link.lookup)>(list[3]);
		g_printsChecked = true;
	} else if (stamp != kKnownStamp) {
		char text[200];
		snprintf(text, sizeof(text), "this is another build of the game (image stamp %08x; the plugin knows %08x)", stamp, kKnownStamp);
		g_linkProblem = text;
		return false;
	} else {
		g_link.tables = reinterpret_cast<decltype(g_link.tables)>(base + kTablesRva);
		g_link.mod    = reinterpret_cast<decltype(g_link.mod)>(base + kModRva);
		g_link.param  = reinterpret_cast<decltype(g_link.param)>(base + kParamRva);
		g_link.lookup = reinterpret_cast<decltype(g_link.lookup)>(base + kLookupRva);
	}
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core == nullptr) {
		g_linkProblem = "D2RCore.dll is not loaded";
		return false;
	}
	g_link.input  = reinterpret_cast<decltype(g_link.input)>(reinterpret_cast<void*>(GetProcAddress(core, "CompileCubeInputField")));
	g_link.output = reinterpret_cast<decltype(g_link.output)>(reinterpret_cast<void*>(GetProcAddress(core, "CompileCubeOutputField")));
	if (g_link.input == nullptr || g_link.output == nullptr) {
		g_linkProblem = "this D2RLoader's D2RCore.dll does not offer CompileCubeInputField / CompileCubeOutputField";
		return false;
	}
	g_link.ok = true;
	return true;
}

// Once the game has read its tables (D2RLoader has made its own changes to the game's code by
// then): is the code the plugin calls and relies on what it was written against?
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
		char found[64] = "";
		for (int i = 0; i < print.size && i < 16; ++i) {
			snprintf(found + i * 3, 4, "%02x ", at[i]);
		}
		g_linkProblem = std::string("the game's code is not what the plugin expects at ") + print.what + " (found " + found + "- another D2RLoader version may keep recipes differently)";
		g_link.ok     = false;
		LogF("off: %s. Recipes stay as the game read them.", g_linkProblem.c_str());
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

// The game's name tables are hash tables: bucket count (a power of two) at +0x18, buckets at
// +0x20 (read by the lookup, 0xa12304). Anything else is not handed to the lookup.
bool LooksLikeNameTable(const void* table) {
	if (!Readable(table, 0x28)) {
		return false;
	}
	uint64_t    buckets = 0;
	const void* first   = nullptr;
	memcpy(&buckets, static_cast<const uint8_t*>(table) + 0x18, sizeof(buckets));
	memcpy(&first, static_cast<const uint8_t*>(table) + 0x20, sizeof(first));
	return buckets != 0 && (buckets & (buckets - 1)) == 0 && buckets <= (1u << 24) && Readable(first, sizeof(void*));
}

// One data version's side of the game, as the recipe compiler sees it.
struct RealGame : cube::Game {
	uint8_t version;
	void*   tables;
	RealGame(uint8_t v, void* t) : version(v), tables(t) {}

	bool Input(char* text, uint8_t* row, int slot, int line) override { return g_link.input(version, text, row, slot, 0, line) != 0; }
	bool Output(char* text, uint8_t* row, int slot, int line) override { return g_link.output(version, text, row, slot, 0, line) != 0; }
	bool Mod(const char* text, uint8_t* row, size_t offset) override {
		g_link.mod(version, text, row, static_cast<int>(offset));
		int16_t id = 0;
		memcpy(&id, row + offset, sizeof(id));
		if (id >= 0) {
			return true;
		}
		// The game leaves -1 for a code no property and no property group has. A property made on
		// Live Properties' Properties tab this session is not in the game's name table until the
		// next start: Live Properties knows its row. The field is the row's number, 32 bits.
		const liveprops::Api* props = liveprops::Find();
		const int             found = props != nullptr && props->findProperty != nullptr ? props->findProperty(text, version) : -1;
		if (found < 0) {
			return false;
		}
		const uint32_t number = static_cast<uint32_t>(found);
		memcpy(row + offset, &number, sizeof(number));
		return true;
	}
	int Class(const char* text) override {
		void* names = *reinterpret_cast<void**>(static_cast<uint8_t*>(tables) + kClassesAt);
		return LooksLikeNameTable(names) ? g_link.lookup(names, text, 0) : -1; // -1: the line is refused, never guessed
	}
	uint32_t Param(const char* text) override {
		uint32_t value = 0;
		g_link.param(version, text, &value);
		return value;
	}
};

// ---------------------------------------------------------------- files

std::wstring Widen(const std::string& text) {
	std::wstring out(text.size(), L'\0');
	const int    length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
	out.resize(static_cast<size_t>(length > 0 ? length : 0));
	return out;
}

std::string Narrow(const std::wstring& text) {
	std::string out(text.size() * 3 + 1, '\0');
	const int   length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
	out.resize(static_cast<size_t>(length > 0 ? length : 0));
	return out;
}

bool IsDirectory(const std::wstring& path) {
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring GameDirectory() {
	wchar_t      path[1024] = {};
	const DWORD  length     = GetModuleFileNameW(nullptr, path, 1000);
	std::wstring out(path, length < 1000 ? length : 0);
	const size_t slash = out.find_last_of(L'\\');
	return slash != std::wstring::npos ? out.substr(0, slash) : out;
}

// The folder the game reads the active mod's loose files from: mods/<mod>/<mod>.mpq/data.
bool FindDataRoot(std::wstring& root, std::string& problem) {
	const std::string modName = g_ctx->activeMod != nullptr ? g_ctx->activeMod : "";
	if (modName.empty()) {
		problem = "no mod is active";
		return false;
	}
	const std::wstring        mod = Widen(modName);
	std::vector<std::wstring> folders;
	if (g_ctx->modDirectory != nullptr && g_ctx->modDirectory[0] != L'\0') {
		std::wstring base = g_ctx->modDirectory;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
			base.pop_back();
		}
		folders.push_back(base + L"\\" + mod + L".mpq");
		folders.push_back(base);
	}
	folders.push_back(GameDirectory() + L"\\mods\\" + mod + L"\\" + mod + L".mpq");
	for (const std::wstring& folder : folders) {
		if (IsDirectory(folder + L"\\data")) {
			root = folder + L"\\data";
			return true;
		}
	}
	problem = "the mod '" + modName + "' has no loose data folder";
	return false;
}

// The whole file, or false while another program holds it or is still writing it.
bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER size {};
	bool          ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart <= 64 * 1024 * 1024;
	if (ok) {
		text.resize(static_cast<size_t>(size.QuadPart));
		size_t done = 0;
		while (ok && done < text.size()) {
			DWORD read = 0;
			ok         = ReadFile(file, text.data() + done, static_cast<DWORD>(text.size() - done), &read, nullptr) != 0 && read != 0;
			done += read;
		}
	}
	CloseHandle(file);
	return ok;
}

// ---------------------------------------------------------------- state

struct Bank {
	uint8_t      version = 0;
	const char*  name    = ""; // the user's file: "recipes.txt" or "recipes-classic-lod.txt"
	std::wstring path;
	void*        tables = nullptr;
	Vector*      vector = nullptr;

	bool        on = false;
	std::string off; // why not

	// the game's own list, as it was when the tables were read, and a copy to build from
	uint8_t*             gameRows  = nullptr;
	uint64_t             gameCount = 0;
	std::vector<uint8_t> game;
	cube::Layout         layout = cube::kWide;
	cube::Cache          cache;

	// the plugin's two lists: one in use, one to build the next in
	uint8_t* buffers[2] = { nullptr, nullptr };
	size_t   capacity   = 0;  // recipes each holds
	int      active     = -1; // which one the game reads; -1 = still the game's own
	uint64_t countSet   = 0;

	// recipe files that went into the game's list (read only): the mod's cubemain.txt and other
	// plugins' copies of it. Their lines are the text of the game's records in all-recipes.txt.
	std::vector<cube::Sheet> sources;
	cube::Cache              listCache;
	std::wstring             listPath;
	std::vector<cube::Shown> book;   // the cube's list as the recipe book shows it
	std::vector<cube::Shown> legend; // legend.txt as the legend panel shows it: the words and codes
	std::vector<cube::Shown> legendValues; // its second tab: the columns and what their values mean
	cube::Names              names;        // the mod's names for item and item type codes
	std::string              mineText;  // the user's file as the cube last took it
	std::string              defaultsText; // the game's own recipes as text (the mod's and the game's), under FullHeader
	std::vector<uint8_t>     textless;     // records of the game no recipe file has text for: always kept
	std::wstring             deletedPath;  // deleted.txt: defaults the user deleted, so that a search does not bring them back
	std::string              deletedText;
	cube::Cache              editCache; // lines compiled for the recipe book's preview
	// the recipe book: the defaults looked up by record (made at its first use), and what was
	// found out about each line shown so far, by the line's text
	struct RowMemo {
		cube::Shown      shown;
		cube::LineStatus status = cube::LineStatus::Blank;
		bool             own = false, isDefault = false;
		std::string      problem;
	};
	bool                                     indexed = false;
	cube::DefaultsIndex                      defaultsIndex;
	std::unordered_map<std::string, RowMemo> rowMemo;

	// from the console command to the game's thread
	bool        pending = false;
	std::string pendingText;

	// for the console
	int                      builds = 0;
	uint64_t                 lastMs = 0;
	std::string              lastLine;
	std::vector<std::string> lastProblems;
	int                      lastRefused = 0; // lines of the last build the game could not read
};

SRWLOCK                  g_lock = SRWLOCK_INIT; // everything below
std::vector<Bank>        g_banks;
bool                     g_setUp      = false;
uint64_t                 g_generation = 0; // counts SetUp()s
std::string              g_setupProblem;
std::wstring             g_folder;         // where the user's files are
std::vector<std::string> g_say;            // lines for the console, written from the UI thread
std::atomic<uint64_t>    g_bookStamp { 0 }; // counts changes of any bank's book

const D2RL::ThreadService* g_threads = nullptr;
std::atomic<bool>          g_wanted { false }; // a job is asked for and has not run yet
std::atomic<uint64_t>      g_askedMs { 0 };

// What Live Tables changed since the last entry (see OnTableChange): taken first thing under the
// lock, so nothing is compiled against an old cache.
void TakeTableChanges();
struct Guard {
	Guard() {
		AcquireSRWLockExclusive(&g_lock);
		TakeTableChanges();
	}
	~Guard() { ReleaseSRWLockExclusive(&g_lock); }
};

void Say(const std::string& text) {
	LogF("%s", text.c_str());
	if (g_config.console) {
		g_say.push_back("Live Cube Recipes: " + text);
	}
}

// ---------------------------------------------------------------- the plugin's own folder

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

// <loader folder>\live-suite\live-cube-recipes\<mod>: beside the plugins folder this DLL was loaded from,
// else the game's d2rloader folder. Never inside a mod's .mpq.
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
		loader = GameDirectory() + L"\\d2rloader";
	}
	const std::string mod = g_ctx->activeMod != nullptr && g_ctx->activeMod[0] != '\0' ? g_ctx->activeMod : "no mod";
	return SuiteFolder(loader, L"live-cube-recipes", Widen(mod), g_moved);
}

void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

// Writes a file that is not there yet; an existing file is never replaced.
void WriteNew(const std::wstring& path, const std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
	CloseHandle(file);
}

// Writes (or replaces) a file the plugin itself makes: first beside it, then moved into place.
bool WriteOwn(const std::wstring& path, const std::string& text) {
	const std::wstring temp = path + L".writing";
	const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool   ok   = true;
	size_t done = 0;
	while (ok && done < text.size()) {
		DWORD written = 0;
		ok            = WriteFile(file, text.data() + done, static_cast<DWORD>(text.size() - done), &written, nullptr) != 0 && written != 0;
		done += written;
	}
	CloseHandle(file);
	ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
	if (!ok) {
		DeleteFileW(temp.c_str());
	}
	return ok;
}

// Recipe files under a loader folder that end in `tail` (other plugins keep changed copies of
// the mod's tables there). Read only. The plugin's own folder, the log folders and the Live
// Suite's exported mods are left out.
void FindCopies(const std::wstring& folder, const std::wstring& tail, int depth, std::vector<std::wstring>& found) {
	WIN32_FIND_DATAW entry {};
	const HANDLE     find = FindFirstFileW((folder + L"\\*").c_str(), &entry);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		const std::wstring name = entry.cFileName;
		const std::wstring path = folder + L"\\" + name;
		if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			if (depth > 0 && name != L"." && name != L".." && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && _wcsicmp(name.c_str(), L"live-cube-recipes") != 0 && _wcsicmp(name.c_str(), L"logs") != 0
				&& _wcsicmp(name.c_str(), L"logs-saved") != 0 && _wcsicmp(name.c_str(), L"plugins") != 0 && _wcsicmp(name.c_str(), L"crashes") != 0
				&& _wcsicmp(name.c_str(), L"mod-export") != 0) { // exported mods (Live Export) are whole mods, not a plugin's copies
				FindCopies(path, tail, depth - 1, found);
			}
		} else if (path.size() >= tail.size() && _wcsicmp(path.c_str() + path.size() - tail.size(), tail.c_str()) == 0 && found.size() < 16) {
			found.push_back(path);
		}
	} while (FindNextFileW(find, &entry) != 0);
	FindClose(find);
}

const char* const kReadMe =
	"Live Cube Recipes: your Horadric Cube recipe list.\r\n"
	"\r\n"
	"recipes.txt              THE CUBE'S RECIPE LIST for Reign of the Warlock characters.\r\n"
	"                         While it is empty, the cube has the recipes of the mod and the game.\r\n"
	"                         Once it holds recipes, the cube has exactly these and no others.\r\n"
	"recipes-classic-lod.txt  the same for classic and Lord of Destruction characters\r\n"
	"deleted.txt              recipes of the mod and the game that you deleted. \"Search for new\r\n"
	"                         recipes\" leaves these out; \"Restore deleted defaults\" brings them back.\r\n"
	"templates\\               whole lists you saved under a name in the recipe book\r\n"
	"all-recipes.txt          every recipe the cube has right now, written by the plugin\r\n"
	"legend.txt               what the words and codes in recipes mean (rar, shld, op 18 ...)\r\n"
	"\r\n"
	"The easy way is the recipe book in game: Ctrl+B (or recipebook in the console).\r\n"
	"  click a recipe          Edit, Copy as new, Disable / Enable, Remove\r\n"
	"  New recipe              a recipe of your own (it goes to the front of the list)\r\n"
	"  Save / Discard          nothing reaches recipes.txt or the cube before Save\r\n"
	"  Search for new recipes  adds recipes of the mod and the game that your list does not have\r\n"
	"                          (after a mod update, for example), except those you deleted\r\n"
	"  Restore deleted defaults  puts every recipe of the mod and the game that you deleted back\r\n"
	"  Templates               keep the list under a name, load a kept list, delete one\r\n"
	"Ctrl+L (or recipelegend) opens the legend on the other side of the screen.\r\n"
	"\r\n"
	"By hand: recipes.txt is a tab-separated table with the columns of the game's cubemain.txt.\r\n"
	"One line is one recipe; the cube tries them from the top and takes the first that fits.\r\n"
	"  add a recipe      add a line            change a recipe   change its line\r\n"
	"  disable a recipe  set enabled to 0      remove a recipe   delete its line\r\n"
	"The first line must keep its *complete column: it says that the file is the whole list.\r\n"
	"After saving the file type recipes in the console (Ctrl + `); the file is read then, and\r\n"
	"once at every start of the game. Nothing looks at it in between.\r\n"
	"\r\n"
	"Every item code, item type, unique, set item and property the running game has loaded can be\r\n"
	"used, the mod's and other plugins' included. A line the game cannot read is left out and named\r\n"
	"in the console and in d2rloader\\logs\\live-cube-recipes.log.\r\n"
	"\r\n"
	"The mod's own files are never changed.\r\n";

// ---------------------------------------------------------------- start: what the game holds

// Read only: compiles a few lines of the mod's own cubemain.txt and looks for exactly those
// records in the game's list, to see that this plugin makes records the way the game does.
// False when lines were tried and none came out as the game has them.
bool CompilerCheck(Bank& bank) {
	std::wstring root;
	std::string  problem, text;
	if (!FindDataRoot(root, problem) || !ReadWhole(root + L"\\global\\excel\\" + (bank.version == 3 ? L"" : L"base\\") + L"cubemain.txt", text)) {
		LogF("%s: the mod has no loose cubemain.txt for data version %u to check the plugin's records against; not checked", bank.name, bank.version);
		return true;
	}
	const cube::Sheet   sheet = cube::ReadSheet(std::move(text));
	RealGame            game(bank.version, bank.tables);
	std::vector<size_t> lines;
	for (size_t i = 0; sheet.usable && i < sheet.lines.size(); ++i) {
		if (!cube::IsBlank(sheet.lines[i])) {
			lines.push_back(i);
		}
	}
	if (lines.empty()) {
		return true;
	}
	const size_t         want = lines.size() < 16 ? lines.size() : 16;
	std::vector<uint8_t> row(cube::kWideRow);
	std::string          firstMiss;
	int                  same = 0, elsewhere = 0, missing = 0, keeps = 0, clears = 0;
	for (size_t s = 0; s < want; ++s) {
		const size_t line  = lines[s * (lines.size() - 1) / (want > 1 ? want - 1 : 1)];
		bool         found = false, inPlace = false;
		for (const bool keep : { false, true }) {
			cube::Layout layout = cube::kWide;
			layout.blankKeeps   = keep;
			bool sensitive      = false;
			if (found || !cube::CompileLine(game, layout, sheet.header, sheet.lines[line], static_cast<int>(line), row.data(), problem, &sensitive)) {
				continue;
			}
			if (line < bank.gameCount && cube::SameRecipe(layout, row.data(), bank.game.data() + line * cube::kWideRow)) {
				found = inPlace = true;
			}
			for (size_t r = 0; !found && r < bank.gameCount; ++r) {
				found = cube::SameRecipe(layout, row.data(), bank.game.data() + r * cube::kWideRow);
			}
			if (found && sensitive) {
				++(keep ? keeps : clears);
			}
		}
		same += inPlace ? 1 : 0;
		elsewhere += found && !inPlace ? 1 : 0;
		missing += found ? 0 : 1;
		if (!found && firstMiss.empty()) {
			firstMiss = cube::Describe(sheet, line);
			if (line < bank.gameCount) { // what differs from the record at the line's own place
				if (cube::CompileLine(game, cube::kWide, sheet.header, sheet.lines[line], static_cast<int>(line), row.data(), problem)) {
					const uint8_t* theirs = bank.game.data() + line * cube::kWideRow;
					int            shown  = 0;
					for (size_t at = 0; at < cube::kWideRow && shown < 12; ++at) {
						if (row[at] != theirs[at] && !cube::IsLineNumberByte(cube::kWide, at)) {
							char part[48];
							snprintf(part, sizeof(part), "%s+0x%zx: %02x, game %02x", shown == 0 ? "; record " : ", ", at, row[at], theirs[at]);
							firstMiss += part;
							++shown;
						}
					}
				} else {
					firstMiss += "; " + problem;
				}
			}
		}
	}
	LogF("%s: check (read only) of %zu lines of the mod's cubemain.txt against the game's own records: %d the same at the line's place, %d the same elsewhere in the list, %d not found", bank.name, want, same,
		elsewhere, missing);
	if (missing != 0) {
		LogF("%s: first line not found: %s", bank.name, firstMiss.c_str());
	}
	bank.layout.blankKeeps = keeps > clears;
	return same + elsewhere != 0;
}

// The mod's cubemain.txt and other plugins' copies of it, as they are now (read only).
void GatherSources(Bank& bank) {
	const std::wstring        tail = std::wstring(L"\\global\\excel\\") + (bank.version == 3 ? L"" : L"base\\") + L"cubemain.txt";
	std::vector<std::wstring> files;
	std::wstring              root;
	std::string               problem;
	if (FindDataRoot(root, problem)) {
		files.push_back(root + tail);
	}
	const std::wstring loader = g_folder.substr(0, g_folder.rfind(L"\\live-cube-recipes\\"));
	FindCopies(loader, tail, 8, files);
	if (g_ctx->modDirectory != nullptr && g_ctx->modDirectory[0] != L'\0' && _wcsicmp((std::wstring(g_ctx->modDirectory) + L"\\d2rloader").c_str(), loader.c_str()) != 0) {
		FindCopies(std::wstring(g_ctx->modDirectory) + L"\\d2rloader", tail, 8, files);
	}
	for (const std::wstring& file : files) {
		std::string text;
		if (ReadWhole(file, text)) {
			bank.sources.push_back(cube::ReadSheet(std::move(text)));
			LogF("%s: recipe text is taken from %s (%zu lines, read only)", bank.name, Narrow(file).c_str(), bank.sources.back().lines.size());
		}
	}
}

// A table of the mod as text: the mod's own file and other plugins' copies of it (read only).
std::vector<std::string> TableTexts(const Bank& bank, const wchar_t* file) {
	const std::wstring        tail = std::wstring(L"\\global\\excel\\") + (bank.version == 3 ? L"" : L"base\\") + file;
	std::vector<std::wstring> files;
	std::wstring              root;
	std::string               problem;
	if (FindDataRoot(root, problem)) {
		files.push_back(root + tail);
	}
	const std::wstring loader = g_folder.substr(0, g_folder.rfind(L"\\live-cube-recipes\\"));
	FindCopies(loader, tail, 8, files);
	std::vector<std::string> texts;
	for (const std::wstring& path : files) {
		std::string text;
		if (ReadWhole(path, text)) {
			texts.push_back(std::move(text));
		}
	}
	return texts;
}

// legend.txt: what the words and codes in recipes mean, from the mod's tables.
void WriteLegend(Bank& bank) {
	const double       start = NowUs();
	cube::LegendTables tables;
	tables.types      = TableTexts(bank, L"itemtypes.txt");
	tables.properties = TableTexts(bank, L"properties.txt");
	tables.stats      = TableTexts(bank, L"itemstatcost.txt");
	for (const wchar_t* file : { L"weapons.txt", L"armor.txt", L"misc.txt" }) {
		for (std::string& text : TableTexts(bank, file)) {
			tables.items.push_back(std::move(text));
		}
	}
	const char* name = bank.version == 3 ? "legend.txt" : "legend-classic-lod.txt";
	bank.names = cube::ReadNames(tables);
	bank.legend.clear();
	bank.legendValues.clear();
	const std::string text = cube::Legend(tables, &bank.legend, &bank.legendValues);
	g_bookStamp.fetch_add(1);
	if (WriteOwn(g_folder + L"\\" + Widen(name), text)) {
		LogF("%s explains the words and codes in recipes (%zu item type, %zu item and %zu property tables read, %.1f ms)", name, tables.types.size(), tables.items.size(), tables.properties.size(),
			(NowUs() - start) / 1000.0);
	} else {
		LogF("%s could not be written", name);
	}
}

// The game's own list as text (the defaults), and the records no recipe file has text for.
void ReadDefaults(Bank& bank) {
	RealGame                        game(bank.version, bank.tables);
	const double                    start = NowUs();
	std::vector<const cube::Sheet*> sources;
	for (const cube::Sheet& sheet : bank.sources) {
		sources.push_back(&sheet);
	}
	const cube::Listing all    = cube::ListAll(game, bank.layout, bank.game.data(), bank.gameCount, sources, bank.listCache);
	const cube::Sheet   listed = cube::ReadSheet(std::string(all.text));
	bank.defaultsText          = cube::FullHeader() + "\r\n";
	bank.textless.clear();
	for (size_t i = 0; i < bank.gameCount && i < all.hasText.size() && i < listed.lines.size(); ++i) {
		if (all.hasText[i] != 0) {
			bank.defaultsText += std::string(listed.lines[i]) + "\r\n";
		} else {
			bank.textless.insert(bank.textless.end(), bank.game.begin() + static_cast<ptrdiff_t>(i * cube::kWideRow), bank.game.begin() + static_cast<ptrdiff_t>((i + 1) * cube::kWideRow));
		}
	}
	bank.deletedPath = g_folder + (bank.version == 3 ? L"\\deleted.txt" : L"\\deleted-classic-lod.txt");
	bank.deletedText.clear();
	static_cast<void>(ReadWhole(bank.deletedPath, bank.deletedText));
	LogF("%s: the game's own list has %zu recipes with text and %zu without (%.1f ms)", bank.name, all.found, all.missing, (NowUs() - start) / 1000.0);
}

// A recipes.txt of before 0.10 held changes to the game's list. It becomes the whole list: the
// game's list with those changes, written out; its removals become notes in deleted.txt.
std::string Migrate(Bank& bank, const cube::Sheet& old) {
	RealGame            game(bank.version, bank.tables);
	const cube::Outcome out = cube::Overlay(game, bank.layout, bank.game.data(), bank.gameCount, old, bank.cache);
	std::vector<const cube::Sheet*> sources { &old };
	for (const cube::Sheet& sheet : bank.sources) {
		sources.push_back(&sheet);
	}
	const cube::Listing all    = cube::ListAll(game, bank.layout, out.rows.data(), out.count, sources, bank.listCache);
	const cube::Sheet   listed = cube::ReadSheet(std::string(all.text));
	std::string         text   = cube::CompleteHeader() + "\r\n";
	for (size_t i = 0; i < all.hasText.size() && i < listed.lines.size(); ++i) {
		if (all.hasText[i] != 0) {
			text += std::string(listed.lines[i]) + "\r\n";
		}
	}
	std::string notes = bank.deletedText.empty() ? cube::FullHeader() + "\r\n" : bank.deletedText;
	int         noted = 0;
	for (size_t i = 0; i < old.lines.size() && i < out.status.size(); ++i) {
		if (out.status[i] == cube::LineStatus::Refused) { // kept, so that it is named and can be put right
			cube::Cells cells = cube::CellsOf(old, i);
			cells.remove      = false;
			text += cube::LineOf(cells) + "\r\n";
		}
		if (out.status[i] == cube::LineStatus::Removed) {
			cube::Cells cells = cube::CellsOf(old, i);
			cells.remove      = false;
			notes += cube::LineOf(cells) + "\r\n";
			++noted;
		}
	}
	WriteNew(bank.path + L".before-0.10", std::string(old.text)); // the old file is kept beside it
	if (WriteOwn(bank.path, text) && (noted == 0 || WriteOwn(bank.deletedPath, notes))) {
		bank.deletedText = noted != 0 ? notes : bank.deletedText;
		LogF("%s held changes to the game's list (the way of before 0.10); it is now the whole list: %zu recipes written, %d removals noted in deleted.txt; the old file is kept as %s.before-0.10", bank.name,
			all.found, noted, bank.name);
	} else {
		LogF("%s: could not be rewritten as a whole list; it is used as one all the same", bank.name);
	}
	return text;
}

// all-recipes.txt: the cube's whole list as text.
void WriteList(Bank& bank, const cube::Sheet& mine, const uint8_t* rows, size_t count, bool atStart) {
	RealGame                        game(bank.version, bank.tables);
	const double                    start = NowUs();
	std::vector<const cube::Sheet*> sources { &mine };
	for (const cube::Sheet& sheet : bank.sources) {
		sources.push_back(&sheet);
	}
	const cube::Listing all = cube::ListAll(game, bank.layout, rows, count, sources, bank.listCache);
	const std::string   name = Narrow(bank.listPath.substr(bank.listPath.find_last_of(L'\\') + 1));
	bank.book = cube::Summaries(cube::ReadSheet(std::string(all.text)));
	g_bookStamp.fetch_add(1);
	char                line[400];
	if (!WriteOwn(bank.listPath, all.text)) {
		snprintf(line, sizeof(line), "%s could not be written (is it open in a program that holds it?)", name.c_str());
		Say(line);
		return;
	}
	snprintf(line, sizeof(line), "%s lists the cube's %zu recipes%s (%.1f ms)", name.c_str(), count,
		all.missing != 0 ? (" - " + std::to_string(all.missing) + " of them have no line in any recipe file and are named only").c_str() : "", (NowUs() - start) / 1000.0);
	if (atStart) {
		LogF("%s", line);
	} else {
		Say(line);
	}
}

void Apply(Bank& bank, bool atStart);
void ReadDefaults(Bank& bank);

// Called when the game has read its tables (start, and D2RLoader's "reload"): everything starts over.
void SetUp() {
	g_banks.clear();
	g_setUp = true;
	++g_generation;
	g_setupProblem.clear();
	if (!g_link.ok || !CheckPrints()) {
		g_setupProblem = g_linkProblem;
		return;
	}
	const D2RL::DataTableService* service = nullptr;
	if (g_ctx->QueryService(&service) != D2RL::ServiceQueryResult::Success || !D2RL::HasDataTableServiceField(service, D2RL::DataTableServiceRequiredSize)) {
		service = nullptr;
	}
	g_folder = OwnFolder();
	if (!g_moved.empty()) {
		LogF("your files were %s", g_moved.c_str());
		g_moved.clear();
	}
	for (uint8_t version = 3; version >= 1; --version) { // RotW first: it is the one people play
		void* tables = g_link.tables(version);
		if (tables == nullptr) {
			continue;
		}
		Vector* vector = reinterpret_cast<Vector*>(static_cast<uint8_t*>(tables) + kVectorAt);
		if (vector->rows == nullptr || vector->count == 0 || vector->count > 1000000) {
			continue;
		}
		bool shared = false;
		for (const Bank& other : g_banks) {
			shared = shared || other.vector == vector || other.gameRows == vector->rows;
		}
		if (shared) {
			continue; // two data versions on one list
		}
		g_banks.emplace_back();
		Bank& bank     = g_banks.back();
		bank.version   = version;
		bank.name      = version == 3 ? "recipes.txt" : "recipes-classic-lod.txt";
		bank.path      = g_folder + L"\\" + Widen(bank.name);
		bank.tables    = tables;
		bank.vector    = vector;
		bank.gameRows  = vector->rows;
		bank.gameCount = vector->count;

		if (service != nullptr) {
			D2RL::DataTables::TableView view {};
			view.structSize = D2RL::DataTables::TableViewSize;
			if (service->getTable(g_ctx, static_cast<D2RL::DataTables::Bank>(version), D2RL::DataTables::TableId::CubeMain, &view) == D2RL::DataTables::Result::Success && view.rowSize != cube::kWideRow) {
				char text[160];
				snprintf(text, sizeof(text), "this D2RLoader keeps recipes of %u bytes; the plugin knows %zu", view.rowSize, cube::kWideRow);
				bank.off = text;
			}
		}
		if (bank.off.empty()) {
			bank.game.assign(bank.gameRows, bank.gameRows + bank.gameCount * cube::kWideRow);
			if (!CompilerCheck(bank)) {
				bank.off = "the plugin's records do not come out as the game's own (see live-cube-recipes.log), so it changes nothing";
			}
		}
		if (!bank.off.empty()) {
			LogF("%s: off: %s", bank.name, bank.off.c_str());
			continue;
		}
		// the user's file: made (first line only) when it is not there
		MakeFolders(g_folder);
		WriteNew(bank.path, cube::FullHeader() + "\r\n");
		static_cast<void>(WriteOwn(g_folder + L"\\README.txt", kReadMe));
		bank.on       = true;
		bank.listPath = g_folder + (version == 3 ? L"\\all-recipes.txt" : L"\\all-recipes-classic-lod.txt");
		GatherSources(bank);
		ReadDefaults(bank);
		WriteLegend(bank);
		LogF("%s: your recipes for data version %u are read from %s (the game holds %llu recipes)", bank.name, bank.version, Narrow(bank.path).c_str(), static_cast<unsigned long long>(bank.gameCount));
		if (!ReadWhole(bank.path, bank.pendingText)) {
			bank.pendingText.clear();
			LogF("%s: could not be read; type recipes in the console to read it again", bank.name);
		}
		Apply(bank, true);
	}
	// The recipe book works on the first list only. The others (classic and Lord of Destruction)
	// are set up and written like it, but need their compiled recipes again only when "recipes"
	// is typed: those are let go, and compiled anew then.
	bool first = true;
	for (Bank& bank : g_banks) {
		if (bank.on && !first) {
			cube::Cache().swap(bank.listCache);
			cube::Cache().swap(bank.cache);
		}
		first = first && !bank.on;
	}
	if (g_banks.empty()) {
		g_setupProblem = "the game holds no cube recipes yet";
		g_setUp        = false; // looked too early: try again
	}
}

// ---------------------------------------------------------------- the user's file becomes the game's list

bool Reserve(Bank& bank, size_t recipes) {
	if (recipes <= bank.capacity) {
		return true;
	}
	// Room to grow; lists the game may still be reading are never freed (two per growth, a few MB).
	const size_t capacity = recipes + recipes / 2 + 1024;
	uint8_t*     first    = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity * cube::kWideRow, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	uint8_t*     second   = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity * cube::kWideRow, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (first == nullptr || second == nullptr) {
		return false;
	}
	if (bank.active >= 0) { // the list in use moves along
		memcpy(first, bank.buffers[bank.active], bank.countSet * cube::kWideRow);
		bank.vector->rows     = first;
		bank.vector->capacity = capacity | kNotOwned;
		bank.active           = 0;
	}
	bank.buffers[0] = first;
	bank.buffers[1] = second;
	bank.capacity   = capacity;
	return true;
}

std::string Count(int count, const char* one, const char* many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

void Apply(Bank& bank, bool atStart) {
	bank.pending = false;
	std::string text;
	text.swap(bank.pendingText);
	if (!bank.on) {
		return;
	}
	// the list must still be the one this plugin knows
	const uint8_t* expectedRows  = bank.active >= 0 ? bank.buffers[bank.active] : bank.gameRows;
	const uint64_t expectedCount = bank.active >= 0 ? bank.countSet : bank.gameCount;
	if (bank.vector->rows != expectedRows || bank.vector->count != expectedCount) {
		bank.on  = false;
		bank.off = "something else replaced the game's recipe list, so the plugin stopped changing it (start the game again, or use D2RLoader's reload)";
		Say(std::string(bank.name) + ": " + bank.off);
		return;
	}
	RealGame          game(bank.version, bank.tables);
	const double      start = NowUs();
	cube::Sheet       mine  = cube::ReadSheet(std::move(text));
	bool              any   = false;
	for (const std::string_view line : mine.lines) {
		any = any || !cube::IsBlank(line);
	}
	if (any && !mine.usable) {
		bank.lastProblems = { "the first line lacks the columns a recipe needs (enabled, numinputs, input 1, output); nothing changed" };
		Say(std::string(bank.name) + ": " + bank.lastProblems[0]);
		if (atStart) {
			WriteList(bank, cube::Sheet {}, bank.game.data(), bank.gameCount, true);
		}
		return;
	}
	if (any && !mine.header.complete) {
		mine = cube::ReadSheet(Migrate(bank, mine));
	}
	bank.mineText = mine.text;
	// a file with recipes is the cube's whole list; an empty one leaves the game's own list
	const size_t  textless = bank.textless.size() / cube::kWideRow;
	cube::Outcome out;
	const bool    whole = mine.header.complete && any; // a file without recipes always means "the game's own"
	if (whole) {
		out = cube::ExactList(game, bank.layout, mine, bank.listCache, bank.textless.data(), textless);
	} else {
		out.rows  = bank.game;
		out.count = bank.gameCount;
		out.status.assign(mine.lines.size(), cube::LineStatus::Blank);
	}
	bank.lastProblems = out.problems;
	++bank.builds;
	bank.lastMs = NowMs();

	std::string what;
	if (whole) {
		what = "the cube has the " + Count(out.added, "recipe", "recipes") + " of this file" + (out.disabled != 0 ? " (" + std::to_string(out.disabled) + " switched off)" : "")
		     + (textless != 0 ? " and " + Count(static_cast<int>(textless), "recipe", "recipes") + " of the game that no file has text for" : "");
	} else {
		what = "no recipes in it, so the cube has the game's own";
	}
	const bool sameAsNow = out.count == expectedCount && memcmp(out.rows.data(), expectedRows, out.count * cube::kWideRow) == 0;
	char       line[500];
	if (sameAsNow) {
		snprintf(line, sizeof(line), "%s: %s; nothing changed since it was last read (%zu recipes).", bank.name, what.c_str(), out.count);
		bank.lastLine = line;
		if (atStart) {
			LogF("%s", line);
		} else {
			Say(line);
		}
	} else {
		// room for the longer of the two lists, so that the list in use never ends before the count does
		if (!Reserve(bank, out.count > expectedCount ? out.count : static_cast<size_t>(expectedCount))) {
			Say(std::string(bank.name) + ": out of memory; nothing changed");
			return;
		}
		const int next = bank.active == 0 ? 1 : 0;
		uint8_t*  to   = bank.buffers[next];
		memcpy(to, out.rows.data(), out.count * cube::kWideRow);
		memset(to + out.count * cube::kWideRow, 0, (bank.capacity - out.count) * cube::kWideRow); // beyond the end: not enabled
		// This runs on the game's own thread, between transmutes. Should another thread be
		// reading all the same (a console command): the game takes the count before the list,
		// so the list is set first; it holds at least as many recipes as the old count and is
		// blank (not enabled) past its end.
		bank.vector->rows     = to;
		bank.vector->capacity = bank.capacity | kNotOwned;
		bank.vector->count    = out.count;
		bank.active           = next;
		bank.countSet         = out.count;
		snprintf(line, sizeof(line), "%s: %s. The cube now knows %zu recipes.", bank.name, what.c_str(), out.count);
		bank.lastLine = line;
		if (atStart) {
			LogF("%s", line);
		} else {
			Say(line);
		}
		LogF("%s: %.1f ms", bank.name, (NowUs() - start) / 1000.0);
	}
	for (const std::string& problem : out.problems) {
		Say(std::string(bank.name) + " " + problem + " - this line is left out until it is fixed");
	}
	for (const std::string& note : out.notes) {
		Say(std::string(bank.name) + " " + note);
	}
	bank.lastRefused = out.refused;
	if (out.refused > static_cast<int>(out.problems.size())) {
		Say(std::string(bank.name) + ": " + std::to_string(out.refused - static_cast<int>(out.problems.size())) + " more lines the game could not read");
	}
	WriteList(bank, mine, out.rows.data(), out.count, atStart);
}

bool         AnyBookOpen();
void __cdecl BookJob(const D2RL::PluginContext*, void*) noexcept;
void         BookAsk(D2RL::Threads::Callback job);

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
	bool say = false;
	{
		Guard guard;
		say = !g_say.empty();
	}
	if (say && g_threads != nullptr && g_threads->runOnUiThread(g_ctx, SayJob, nullptr) != D2RL::Threads::Result::Success) {
		Guard guard;
		g_say.clear(); // no console to write to; the lines are in the log
	}
}

void __cdecl Job(const D2RL::PluginContext*, void*) noexcept {
	{
		Guard guard;
		g_wanted.store(false);
		if (!g_setUp) {
			SetUp();
		}
		for (Bank& bank : g_banks) {
			if (bank.pending) {
				Apply(bank, false);
			}
		}
	}
	FlushSay();
	if (AnyBookOpen()) {
		BookAsk(BookJob); // an open book shows the new list
	}
}

// A job asked for on the game's thread does not run while no game runs. This one goes round on
// the UI's thread (once per UI update) and runs Job there when it has waited 1.5 s in vain.
void __cdecl Later(const D2RL::PluginContext*, void*) noexcept {
	if (!g_wanted.load()) {
		return; // done on the game's thread
	}
	if (NowMs() - g_askedMs.load() > 1500) {
		Job(g_ctx, nullptr);
	} else if (g_threads->runOnUiThread(g_ctx, Later, nullptr) != D2RL::Threads::Result::Success) {
		Job(g_ctx, nullptr);
	}
}

// Asks for Job on the game's own thread (where the cube runs); without one, on the UI's.
void Ask() {
	g_wanted.store(true);
	g_askedMs.store(NowMs());
	if (g_threads->runOnGameThread(g_ctx, Job, nullptr) != D2RL::Threads::Result::Success) {
		if (g_threads->runOnUiThread(g_ctx, Job, nullptr) != D2RL::Threads::Result::Success) {
			Job(g_ctx, nullptr);
		}
	} else {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, Later, nullptr)); // in case no game is running
	}
}

// ---- Live Tables (../Live Tables/live_tables_api.h): stat and property rows that come and go during play
//
// A recipe record holds row numbers: a mod cell the property's, a param cell naming a stat the
// stat's. The compiled lines kept between builds (listCache, editCache, the book's rowMemo) hold
// those numbers and the refusals ("not a property code"). When Live Tables grows a table, a line
// refused for a code that did not exist yet can be read now; when it takes a row out (Shrink,
// Remove), numbers above it are one lower. Its change listener only records which (no lock, no
// allocation: it runs in Live Tables' growth job) and asks for a job; the next entry under the
// lock lets the kept lines go, and the job compiles the cube's list again from the text the cube
// last took - only when numbers moved, or a line of the last build was refused.
std::atomic<uint32_t> g_tableChange { 0 }; // 1 = a row came, 2 = numbers moved, 4 = a row was renamed
std::atomic<bool>     g_tablesHeard { false };

// Ask() without its last resort (running Job here): from Live Tables' job no lock may be taken.
void AskSoon() {
	g_wanted.store(true);
	g_askedMs.store(NowMs());
	if (g_threads == nullptr) {
		return; // the next entry takes the change
	}
	if (g_threads->runOnGameThread(g_ctx, Job, nullptr) == D2RL::Threads::Result::Success) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, Later, nullptr)); // in case no game is running
	} else {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, Job, nullptr));
	}
}

// Renames (Live Tables API 3, Change kind Rename): the listener copies the two names into this
// ring (one writer: Live Tables' job; read under the lock), no allocation. The next entry rewrites
// the code in the recipe files (after a dated backup) and the recipe book's unsaved rows.
struct Renamed {
	int  table = 0, version = 0;
	char oldName[64] = "", newName[64] = "";
};
constexpr size_t      kRenameRoom = 16;
Renamed               g_renames[kRenameRoom];
std::atomic<size_t>   g_renameHead { 0 }, g_renameTail { 0 }; // written / taken
std::atomic<int>      g_renamesLost { 0 };
std::vector<Renamed>  g_editorRenames; // under the lock: for the book's rows, done on the UI thread

void RecordRename(const livetables::Change* change) {
	if (change->size < offsetof(livetables::Change, newName) + sizeof(change->newName) || change->oldName == nullptr || change->newName == nullptr) {
		return;
	}
	const size_t head = g_renameHead.load();
	if (head - g_renameTail.load() >= kRenameRoom || strlen(change->oldName) >= sizeof(Renamed::oldName) || strlen(change->newName) >= sizeof(Renamed::newName)) {
		g_renamesLost.fetch_add(1);
		return;
	}
	Renamed& slot = g_renames[head % kRenameRoom];
	slot.table    = static_cast<int>(change->table);
	slot.version  = change->dataVersion;
	strcpy(slot.oldName, change->oldName);
	strcpy(slot.newName, change->newName);
	g_renameHead.store(head + 1);
}

void __cdecl OnTableChange(void*, const livetables::Change* change) noexcept {
	if (change == nullptr || change->size < offsetof(livetables::Change, revision) + sizeof(change->revision)) {
		return;
	}
	uint32_t bits = 0;
	switch (change->kind) {
	case livetables::ChangeKind::Grow: bits = 1; break;
	case livetables::ChangeKind::Shrink:
	case livetables::ChangeKind::Remove: bits = 2; break;
	case livetables::ChangeKind::Rename:
		RecordRename(change);
		bits = 4;
		break;
	default: return; // Load: SetUp starts over anyway; Disable / Enable keep every number
	}
	g_tableChange.fetch_or(bits);
	AskSoon();
}

int __cdecl CanRemove(void*, livetables::Table table, int dataVersion, int number, char* why, size_t whySize) noexcept;
struct Bank;
Bank* ShownBank();

// Live Tables API 1 (0.1.x) has the listener already; a version 2 DLL answers a version 1 caller too.
void ListenToTables() {
	if (g_tablesHeard.load()) {
		return;
	}
	const HMODULE module = GetModuleHandleW(L"d2rl-live-tables.dll");
	const auto    get    = module != nullptr ? reinterpret_cast<const livetables::Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "LiveTablesGetApi"))) : nullptr;
	const livetables::Api* lt = get != nullptr ? get(1) : nullptr;
	if (lt == nullptr || lt->size < offsetof(livetables::Api, removeListener) + sizeof(lt->removeListener) || lt->addListener == nullptr) {
		return;
	}
	const int handle = lt->addListener(OnTableChange, nullptr);
	g_tablesHeard.store(handle > 0);
	// version 3: the remove veto (asked again for the full table, a version 1 DLL has no such field)
	const livetables::Api* v3 = get(3);
	if (v3 != nullptr && v3->version >= 3 && v3->size >= offsetof(livetables::Api, removeVeto) + sizeof(v3->removeVeto) && v3->addVeto != nullptr) {
		const int veto = v3->addVeto(CanRemove, nullptr);
		LogF("Live Tables: %s", veto > 0 ? "a stat or property row your recipe files name cannot be removed (remove veto on)" : "its remove veto REFUSED");
	}
	LogF("Live Tables: found (API %u); %s", lt->version,
		handle > 0 ? "a property or stat row made or taken out during play is read into the recipes at once" : "its change listener REFUSED - a recipe line refused for a property made later is read again when you type recipes");
}

// Live Tables' remove veto (API 3): remove() asks it first, on the thread calling remove(), with
// none of Live Tables' locks held. The cube's own records Live Tables checks itself; this looks at
// the recipe TEXT, which a record may not show yet: recipes.txt as it is on disk now, the text the
// cube last took, and the recipe book's unsaved rows. A line that names the row (or, for a middle
// row, any row after it, which would move) refuses the removal, naming the line.
std::atomic<DWORD> g_uiThread { 0 }; // the thread the recipe book runs on (seen in its jobs)
bool               EditorHasChanges();
std::string        EditorText(); // the book's rows as a file (UI thread only)

int __cdecl CanRemove(void*, livetables::Table table, int dataVersion, int number, char* why, size_t whySize) noexcept {
	{
		if (number < 0 || (table != livetables::Table::Properties && table != livetables::Table::ItemStatCost)) {
			return 1;
		}
		const bool  stats = table == livetables::Table::ItemStatCost;
		std::string found;
		{
			Guard guard;
			for (Bank& bank : g_banks) {
				const bool sameVersion = bank.version == dataVersion || dataVersion == 0 || (bank.version <= 2 && dataVersion <= 2); // 1 and 2: one bank
				if (!bank.on || !sameVersion) {
					continue;
				}
				std::vector<std::pair<std::string, std::string>> texts; // what, text
				std::string onDisk;
				if (ReadWhole(bank.path, onDisk)) {
					texts.emplace_back(bank.name, std::move(onDisk));
				}
				texts.emplace_back(std::string(bank.name) + " (as the cube took it)", bank.mineText);
				if (&bank == ShownBank() && EditorHasChanges()) {
					if (GetCurrentThreadId() != g_uiThread.load()) {
						found = "the recipe book has unsaved changes; save or discard them first";
						break;
					}
					texts.emplace_back("the recipe book (not saved)", EditorText());
				}
				RealGame             game(bank.version, bank.tables);
				std::vector<uint8_t> row(cube::kWideRow);
				std::string          problem;
				for (const auto& [what, text] : texts) {
					const cube::Sheet sheet = cube::ReadSheet(std::string(text));
					if (!sheet.usable) {
						continue;
					}
					for (size_t i = 0; i < sheet.lines.size() && found.empty(); ++i) {
						if (cube::IsBlank(sheet.lines[i]) || !cube::CompileLine(game, bank.layout, sheet.header, sheet.lines[i], static_cast<int>(i), row.data(), problem)) {
							continue;
						}
						const int column = cube::NamesRow(bank.layout, row.data(), stats, static_cast<uint32_t>(number));
						if (column >= 0) {
							found = what + ": " + cube::Describe(sheet, i) + " names " + (stats ? "stat " : "property ") + std::to_string(number)
							      + " or a row after it in " + cube::kColumns[column].name + "; change or remove that line first";
						}
					}
					if (!found.empty()) {
						break;
					}
				}
				if (!found.empty()) {
					break;
				}
			}
		}
		if (found.empty()) {
			return 1;
		}
		LogF("Live Tables asked to remove %s row %d (data version %d): refused - %s", stats ? "stat" : "property", number, dataVersion, found.c_str());
		if (why != nullptr && whySize != 0) {
			snprintf(why, whySize, "Live Cube Recipes: %s", found.c_str());
		}
		return 0;
	}
}

std::string StampNow() {
	SYSTEMTIME now {};
	GetLocalTime(&now);
	char text[32];
	snprintf(text, sizeof(text), "%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	return text;
}

void EditorRenameJob(const D2RL::PluginContext*, void*) noexcept;

// A rename in the recipe files: recipes.txt (or the classic one) is the suite's own file in
// live-suite, so the code in it follows the row (lead-71, 2026-11-25): a dated copy first
// (recipes.before-rename-<date>-<time>.txt), then the file with every cell that named the old code
// rewritten, nothing else changed. The text the cube took is rewritten the same way (its records
// keep the row's number, so the cube needs nothing).
// One file of the plugin's folder: a dated copy first (`copy`), then every cell that named the old
// code rewritten; a log line either way it changed. `shown` names the file in messages.
void RenameFile(const std::wstring& path, const std::wstring& copy, const std::string& shown, bool stats, const Renamed& r) {
	const char*              kind = stats ? "stat" : "property";
	std::string              text, before;
	std::vector<std::string> where;
	if (!ReadWhole(path, text)) {
		return; // no such file
	}
	before          = text;
	const int cells = cube::RenameInText(text, stats, r.oldName, r.newName, &where);
	if (cells == 0) {
		LogF("%s: %s %s is now %s - no line names it", shown.c_str(), kind, r.oldName, r.newName);
		return;
	}
	const size_t slash = copy.find_last_of(L'\\');
	if (slash != std::wstring::npos) {
		MakeFolders(copy.substr(0, slash));
	}
	const bool  kept  = WriteOwn(copy, before);
	const bool  wrote = kept && WriteOwn(path, text);
	std::string places;
	for (size_t i = 0; i < where.size() && i < 5; ++i) {
		places += (i == 0 ? "" : ", ") + where[i];
	}
	if (where.size() > 5) {
		places += ", ...";
	}
	const std::string copyShown = Narrow(copy.substr(g_folder.size() + 1));
	if (wrote) {
		LogF("%s: %s %s is now %s - %d %s rewritten (%s); the file before: %s", shown.c_str(), kind, r.oldName, r.newName, cells, cells == 1 ? "cell" : "cells", places.c_str(), copyShown.c_str());
	} else {
		Say(shown + ": " + kind + " " + r.oldName + " is now " + r.newName + ", but the file could not be " + (kept ? "written" : "copied first") + " (another program holds it?); change " + places + " by hand");
	}
}

// The recipe files of the plugin's folder: recipes.txt (or the classic one), deleted.txt (the
// defaults the user deleted) and, for the first list, every template.
void TakeRenames() {
	if (const int lost = g_renamesLost.exchange(0); lost != 0) {
		LogF("Live Tables: %d renames came with names too long or too many at once; the recipe files were not rewritten for them", lost);
	}
	bool forBook = false;
	while (g_renameTail.load() != g_renameHead.load()) {
		const Renamed r = g_renames[g_renameTail.load() % kRenameRoom];
		g_renameTail.fetch_add(1);
		const bool        stats = r.table == static_cast<int>(livetables::Table::ItemStatCost);
		const std::string stamp = StampNow();
		for (Bank& bank : g_banks) {
			const bool sameVersion = bank.version == r.version || r.version == 0 || (bank.version <= 2 && r.version <= 2);
			if (!bank.on || !sameVersion) {
				continue;
			}
			const auto stemOf = [](const std::wstring& path) {
				const size_t slash = path.find_last_of(L'\\'), dot = path.rfind(L'.');
				return path.substr(0, dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? dot : path.size());
			};
			const std::wstring when = L".before-rename-" + Widen(stamp) + L".txt";
			RenameFile(bank.path, stemOf(bank.path) + when, bank.name, stats, r);
			RenameFile(bank.deletedPath, stemOf(bank.deletedPath) + when, Narrow(bank.deletedPath.substr(g_folder.size() + 1)), stats, r);
			static_cast<void>(ReadWhole(bank.deletedPath, bank.deletedText));
			cube::RenameInText(bank.mineText, stats, r.oldName, r.newName, nullptr);
			if (bank.pending) {
				cube::RenameInText(bank.pendingText, stats, r.oldName, r.newName, nullptr);
			}
			if (&bank == ShownBank()) {
				// templates: their copies go to templates\before-rename\, out of the book's list of templates
				WIN32_FIND_DATAW entry {};
				const HANDLE     find = FindFirstFileW((g_folder + L"\\templates\\*.txt").c_str(), &entry);
				if (find != INVALID_HANDLE_VALUE) {
					do {
						if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
							const std::wstring file = entry.cFileName;
							RenameFile(g_folder + L"\\templates\\" + file, g_folder + L"\\templates\\before-rename\\" + stemOf(file) + when, "templates\\" + Narrow(file), stats, r);
						}
					} while (FindNextFileW(find, &entry) != 0);
					FindClose(find);
				}
				g_editorRenames.push_back(r);
				forBook = true;
			}
		}
	}
	if (forBook) {
		BookAsk(EditorRenameJob);
	}
}

void TakeTableChanges() {
	const uint32_t bits = g_tableChange.exchange(0);
	if (bits == 0 || !g_setUp) {
		return;
	}
	if ((bits & 4) != 0) {
		TakeRenames();
	}
	int again = 0;
	for (Bank& bank : g_banks) {
		cube::Cache().swap(bank.listCache);
		cube::Cache().swap(bank.editCache);
		cube::Cache().swap(bank.cache);
		bank.rowMemo.clear();
		bank.defaultsIndex = cube::DefaultsIndex {}; // it pointed into listCache
		bank.indexed       = false;
		if (bank.on && !bank.pending && ((bits & 2) != 0 || ((bits & 1) != 0 && bank.lastRefused > 0))) {
			bank.pending     = true;
			bank.pendingText = bank.mineText;
			++again;
		}
	}
	if ((bits & 3) != 0) {
		LogF("Live Tables: %s; %s", (bits & 2) != 0 ? "a row was taken out (numbers moved)" : "a row was made",
			again != 0 ? "the recipes are compiled again" : "no recipe line waited for it");
	}
}

void BookSetUp();
void SectionsChanged();
extern std::string g_bookOff;

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	ListenToTables(); // Live Tables may load after this plugin
	{
		Guard guard;
		g_tableChange.store(0); // the tables are read anew: everything starts over
		SetUp();
	}
	BookSetUp();
	SectionsChanged(); // the legend's entries are there now
	if (!g_bookOff.empty()) {
		LogF("panels: not available - %s", g_bookOff.c_str());
	}
	FlushSay();
}

// ---------------------------------------------------------------- the two books (panels)
//
// The recipe book (left side of the screen: the cube's list, the text of all-recipes.txt) and
// the legend (right side: the entries of legend.txt). Both are panels of the game's own kind:
// a side panel's frame, hinge and title (as the quest log), the stash's page arrows. Each shows
// kBookRows entries a page, filtered by the words typed. Panel calls run on the UI thread.
//
// D2RLoader 1.3.1's widget service cannot change a label or wire a button, its panels know no
// text entry and it has no key listener. So, as RuffnecKk's MapSense does on this loader:
//  * a page's text is written into the layout itself. A book is a pool of panels; to show
//    another page the next panel of the pool gets a new layout and is opened, then the one
//    before is closed. A closed panel is unloaded ("PanelManager:UnloadPanel") before it is
//    used again, and not sooner than 1.5 s after it closed;
//  * a button's click is a "PanelManager:ClosePanel" message for a panel name that does not
//    exist, which the UI message listener takes as the click;
//  * a row's whole text is the hover text of an unseen button over the row;
//  * while a book is open a low-level keyboard and mouse hook takes the keys that are text
//    (so the game's hotkeys do not fire), Backspace, Page Up/Down, Home/End and the wheel.
//    With both books open they go to the one the cursor is over.
//
// The recipe book is also the editor of the user's recipes.txt. It shows the list as it will
// be with the changes made so far; nothing reaches the file or the cube before "Save".


// Text broken into lines of at most `most` letters at spaces, for a hover box.
std::string Wrapped(const std::string& text, size_t most) {
	std::string out;
	size_t      line = 0;
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find(' ', at);
		end        = end == std::string::npos ? text.size() : end;
		if (line != 0 && line + 1 + (end - at) > most) {
			out += '\n';
			line = 0;
		} else if (line != 0) {
			out += ' ';
			++line;
		}
		out.append(text, at, end - at);
		line += end - at;
		at = end;
		while (at < text.size() && text[at] == ' ') {
			++at;
		}
	}
	return out;
}

// Text cut with "..." when longer than the panel has room for.
std::string Fit(const std::string& text, size_t most) {
	return text.size() <= most ? text : text.substr(0, most - 3) + "...";
}

enum class View { List, Card, Edit, Prompt, Confirm, Templates, TemplateName };
enum class Asked { Search, Restore, LoadTemplate, DeleteTemplate, RemoveRecipe };

// Keys that are not letters, as they stand in Book::keys.
constexpr char kKeyEnter = '\r', kKeyBack = '\x01', kKeyUp = '\x11', kKeyDown = '\x12', kKeySave = '\x13', kKeyNew = '\x0e', kKeyPaste = '\x16', kKeyCopy = '\x03', kKeyUndo = '\x1a', kKeyRedo = '\x19';
constexpr int  kWheelRows = 2; // entries a notch of the wheel moves a list

struct Book {
	const char* id;      // "RecipeBook" / "Legend": panel names, click names
	const char* heading; // the panel's title
	const char* hint;    // in the empty search slot
	bool        right;   // on the right side of the screen
	bool        legend;  // shows the legend's entries, not the recipes
	int         rows;    // entries a page

	// asked for from other threads (the hooks, the UI message listener); keys and clicks under g_typeLock
	std::atomic<bool>        open { false }; // believed open (cleared when found closed)
	std::atomic<int>         move { 0 };     // pages to go
	std::atomic<int>         scroll { 0 };   // rows to go (the wheel)
	std::atomic<int>         jump { 0 };     // -1 first page, 1 last page
	std::atomic<bool>        redo { false }; // the page must be drawn again
	std::string              keys;           // typed: letters, '\b' backspace, '\x7f' delete, '\t' next cell
	std::vector<std::string> clicks;         // "Row3", "Save", "Escape" ...

	// UI thread only
	std::vector<cube::Shown> entries;
	uint64_t                 stamp = ~0ull;
	std::string              words;
	std::vector<uint32_t>    found;
	size_t                   top = 0; // the first entry shown
	int                      tab    = 0;  // the legend: 0 words and codes, 1 columns and values
	int                      handle[2] = { 0, 0 }; // its tabs in the Mod Book (the legend has two)
};
Book    g_recipeBook { "RecipeBook", "Cube Recipes", "Type to search the recipes", false, false, 8 };
Book    g_legendBook { "Legend", "Recipe Legend", "Type a word or code: rar, shld, r10", true, true, 9 };
Book*   g_books[]  = { &g_recipeBook, &g_legendBook };
SRWLOCK g_typeLock = SRWLOCK_INIT;

// The editor: the user's list as it will be saved, and what the recipe book shows of it.
// recipes.txt is the cube's whole list. A fresh (empty) file starts as the game's own list.
struct Editor { // UI thread only
	std::vector<cube::Cells>      rows;        // the list, line by line (entry n of the book is row n)
	std::vector<cube::Cells>      deleted;     // defaults the user deleted (deleted.txt)
	int                           changes = 0; // not saved yet
	// Undo: what a change took out of the list, to be put back. A step taken back becomes a step
	// of `redo`. Both are emptied by Save, Discard and a new reading of the file.
	struct Step {
		size_t                   at    = 0; // where in the rows
		std::vector<cube::Cells> rows;      // the rows that stood there before
		size_t                   count = 0; // how many rows stand there now
		std::vector<cube::Cells> deleted;   // the notes as they were
		int                      changes = 0;
	};
	std::vector<Step> undo, redo;
	View                          view    = View::List;
	View                          promptFrom = View::List;
	bool                          closeAfterPrompt = false;
	std::vector<cube::LineStatus> status;    // per row
	std::vector<std::string>      problemOf; // per row: why the game cannot read it
	std::vector<uint8_t>          own;       // per row: changed or made by the user
	std::vector<uint8_t>          isDefault; // per row: a default has these ingredients
	// the recipe on the card
	int         entry = -1;
	// the recipe in the editor
	cube::Cells edit, original;
	int         editRow = -1;        // the row being edited, or -1: a new row is made
	std::vector<int> fields;         // the cells in the editor's order: -1 the description, else a column
	int         field = 0;           // the cell typed into
	int         fieldTop = 0;        // the first cell shown
	bool        allCells = false;    // every cell, not only the usual ones and those filled in
	std::string problem;
	// what a confirmation is asked for
	Asked                    asked = Asked::Search;
	std::vector<cube::Cells> confirmRows; // the recipes to add (search, restore) or the template's list (load)
	// templates: whole lists saved under a name, in the templates folder
	struct Template {
		std::string name;
		size_t      recipes = 0;
	};
	std::vector<Template> templates;
	int                   templatePick = -1; // the one a confirmation is about
	std::string           templateName;      // the name being typed
	std::string           templateProblem;
};
Editor g_editor;

// ---- layouts

const modbook::Api* g_book = nullptr;       // the Mod Book plugin's interface; nullptr: not there
modbook::Page*      g_page = nullptr;       // the page being drawn (inside a tab's draw only)
std::string         g_bookOff = "not set up"; // why there are no books

// A page's widgets. The frame, the title's place, the buttons' pictures and the click messages
// are the Mod Book's; this hands it the texts and places.
struct LayoutText {
	const Book& book;
	int         next = 0, rowY = modbook::kActionY; // an older Mod Book: where the next action goes
	LayoutText(const Book& b, const std::string&) : book(b) {}
	// The tab's own buttons in rows under the tab rows (the Mod Book's action row: the same places
	// on every tab; slots 232 wide, 4 to a row). An older Mod Book: the same places, by hand.
	// The answer: the first y under the rows so far.
	int action(const std::string& click, const std::string& text, const char* color, int slots) {
		// The most letters a label can have in a button of 1, 2, 3, 4 slots before it wraps out of it
		// (Mod Book's finding: 12 in one slot, 28 in two; three and four slots counted the same way).
		static const size_t most[] = { 12, 28, 36, 38 };
		const int           at     = slots < 1 ? 0 : slots > 4 ? 3 : slots - 1;
		if (text.size() > most[at]) {
			LogF("layout: the button label \"%s\" is too long for %d slot(s) (at most %zu letters)", text.c_str(), at + 1, most[at]);
		}
		if (modbook::HasRows(g_book)) {
			return g_book->action(g_page, click.c_str(), text.c_str(), color, slots);
		}
		slots = slots < 1 ? 1 : slots > modbook::kActionsInRow ? modbook::kActionsInRow : slots;
		if (next + slots > modbook::kActionsInRow) {
			next = 0;
			rowY += modbook::kActionRowStep;
		}
		static const int widths[] = { 232, 470, 700, 742 };
		button(click, 82 + next * (modbook::kActionSlot + modbook::kActionGap), rowY, widths[slots - 1], text, color != nullptr ? color : "$FontColorGoldYellow");
		next += slots;
		return rowY + modbook::kActionRowStep;
	}
	// Ends the row: the next action starts a new one (so that a button that comes and goes does
	// not move the ones after it).
	int endRow() {
		if (modbook::HasRows(g_book)) {
			return g_book->action(g_page, nullptr, nullptr, nullptr, 0);
		}
		if (next != 0) {
			next = 0;
			rowY += modbook::kActionRowStep;
		}
		return rowY;
	}
	// The search box, where every tab has it.
	void search(const std::string& typed, const std::string& hint) {
		if (modbook::HasRows(g_book)) {
			g_book->search(g_page, typed.c_str(), hint.c_str());
			return;
		}
		slot("SearchSlot", 210, 138);
		label("Search", 250, 166, 660, 56, typed.empty() ? "$FontColorGoldYellow" : "$FontColorWhite", "left", false, typed.empty() ? hint : Fit(typed, 34) + "_");
	}
	void title(const std::string& text) { g_book->title(g_page, text.c_str()); }
	void label(const std::string& name, int x, int y, int width, int height, const char* color, const char* align, bool wrap, const std::string& text) {
		g_book->label(g_page, name.c_str(), x, y, width, height, color, align, wrap ? 1 : 0, text.c_str());
	}
	// An unseen button over a place: a click sends `click` (when not empty), hovering shows `tip`.
	void spot(const std::string& name, int x, int y, int width, int height, const std::string& click, const std::string& tip) {
		g_book->spot(g_page, name.c_str(), x, y, width, height, click.c_str(), tip.c_str());
	}
	void button(const std::string& click, int x, int y, int width, const std::string& text, const char* color = "$FontColorWhite") {
		g_book->button(g_page, click.c_str(), x, y, width, text.c_str(), color);
	}
	void slot(const char* name, int x, int y) { g_book->image(g_page, name, x, y, "PANEL\\Modals\\InputSlot"); }
	// Where the list stands: a thin line down the right side with a mark on it.
	void bar(int y, size_t top, size_t shown, size_t count) { g_book->bar(g_page, y, top, shown, count); }
	void pager(bool before, bool after, const std::string& text) { g_book->pager(g_page, before ? 1 : 0, after ? 1 : 0, text.c_str()); }
	std::string done() { return {}; }
};

// The list: a search slot, the entries of one page, the arrows; in the recipe book also the
// editor's bar (New, and Save / Discard when there are changes) and rows that can be clicked.
std::string ListLayout(const Book& b, const std::string& panelName) {
	const size_t rows = static_cast<size_t>(b.rows);
	LayoutText   l(b, panelName);
	l.title(b.heading);
	l.search(b.words, b.legend && b.tab == 1 ? "Type: op 18, param, stat 370, plvl" : b.hint);
	int top = 268; // the legend (a Mod Book before 0.8.0): under the Mod Book's row of tabs (its two tabs are tabs there)
	if (!b.legend) {
		if (!g_editor.undo.empty()) { // beside the search slot
			l.button("Undo", 86, 172, 116, "Undo");
		}
		l.action("New", "New recipe", "$FontColorWhite", 1);
		l.action("Templates", "Templates", "$FontColorWhite", 1);
		if (g_editor.changes != 0) {
			l.action("Save", "Save (" + std::to_string(g_editor.changes) + ")", "$FontColorGreen", 1); // the changes not saved yet
			l.action("Discard", "Discard", "$FontColorLightRed", 1);
		}
		l.endRow();
		l.action("Search", "Search for new recipes", "$FontColorLightTeal", 2);
		top = l.action("RestoreAll", "Restore deleted defaults", "$FontColorLightTeal", 2);
	}
	for (size_t i = 0; i < rows; ++i) {
		const size_t at    = b.top + i;
		const bool   there = at < b.found.size();
		const int    y     = top + static_cast<int>(i) * 106;
		// the frame has room for about 58 letters a line at this size
		l.label("Title" + std::to_string(i), 82, y, 990, 36, "$FontColorLightGold", "left", false, there ? Fit(b.entries[b.found[at]].title, 58) : "");
		l.label("Detail" + std::to_string(i), 120, y + 36, 950, 66, "$FontColorGoldYellow", "left", true, there ? Fit(b.entries[b.found[at]].detail, 108) : "");
		if (there) { // the whole entry when the cursor is on it; in the recipe book a click opens it
			l.spot("Hover" + std::to_string(i), 82, y, 980, 102, "Row" + std::to_string(i),
				Wrapped(b.entries[b.found[at]].title, 64) + "\n" + Wrapped(b.entries[b.found[at]].detail, 64) + (b.legend ? "\n(click: puts its code into the cell you are editing)" : "\n(click to open)"));
		}
	}
	char text[200];
	if (b.found.empty()) {
		snprintf(text, sizeof(text), b.entries.empty() ? "nothing read yet" : "nothing has all of these words");
	} else {
		snprintf(text, sizeof(text), "%zu - %zu of %zu", b.top + 1, b.top + rows < b.found.size() ? b.top + rows : b.found.size(), b.found.size());
	}
	l.bar(top, b.top, rows, b.found.size());
	l.pager(b.top > 0, b.top + rows < b.found.size(), text);
	return l.done();
}

const char* CellName(int field) {
	return field < 0 ? "description" : cube::kColumns[field].name;
}
std::string& CellText(cube::Cells& cells, int field) {
	return field < 0 ? cells.description : cells.cell[static_cast<size_t>(field)];
}

// One recipe with every cell that has something in it, and what can be done with it.
std::string CardLayout(const Book& b, const std::string& panelName) {
	const Editor&      e      = g_editor;
	const size_t       row    = static_cast<size_t>(e.entry);
	const cube::Cells& cells  = e.rows[row];
	const bool         unread = e.status[row] == cube::LineStatus::Refused;
	const bool         off    = cells.cell[0] == "0";
	LayoutText         l(b, panelName);
	l.title("Recipe");
	l.action("Back", "Back", "$FontColorWhite", 1);
	l.action("Edit", "Edit", "$FontColorWhite", 1);
	l.action("Copy", "Copy as new", "$FontColorWhite", 1);
	l.endRow();
	if (!unread) {
		l.action("Toggle", off ? "Enable" : "Disable", "$FontColorWhite", 1);
	}
	const int under = l.action("Remove", "Remove", "$FontColorLightRed", 1);
	const std::string what = unread ? "The game cannot read it: " + e.problemOf[row]
	                                : std::string(e.own[row] != 0 ? "Changed or made by you." : "As the mod has it.") + (off ? " It is switched off." : "")
	                                      + (e.problemOf[row].empty() ? std::string() : " Note: " + e.problemOf[row] + ".");
	const int whatHigh = what.size() > 90 ? 110 : 70; // a note takes a second line
	l.label("What", 82, under, 990, whatHigh, "$FontColorLightTeal", "left", true, what);
	int y = under + whatHigh + 8, shown = 0;
	for (int field = -1; field < static_cast<int>(cube::kColumnCount) && y + 40 <= 1340; ++field) {
		const std::string text = field < 0 ? cells.description : cells.cell[static_cast<size_t>(field)];
		if (text.empty()) {
			continue;
		}
		l.label("Name" + std::to_string(shown), 82, y, 290, 40, "$FontColorGoldYellow", "left", false, CellName(field));
		l.label("Value" + std::to_string(shown), 380, y, 690, 40, "$FontColorLightGold", "left", false, Fit(text, 40));
		if (text.size() > 40) {
			l.spot("Whole" + std::to_string(shown), 380, y, 690, 40, "", Wrapped(text, 64));
		}
		y += 41;
		++shown;
	}
	return l.done();
}

constexpr int kEditRows = 10; // the cells shown at a time, under the action row and the help

// The cells of one recipe, twelve at a time; a click picks the cell that is typed into.
std::string EditLayout(const Book& b, const std::string& panelName) {
	const Editor& e     = g_editor;
	const size_t  top   = static_cast<size_t>(e.fieldTop);
	LayoutText    l(b, panelName);
	l.title(e.editRow < 0 ? "New Recipe" : "Edit Recipe");
	l.action("Done", "Done", "$FontColorGreen", 1);
	l.action("Cancel", "Cancel", "$FontColorLightRed", 1);
	l.action("Name", "Name it", "$FontColorLightTeal", 1);
	const int under   = l.action("Cells", e.allCells ? "Fewer cells" : "All cells", "$FontColorWhite", 1);
	const int kEditTop = under + 90; // the first cell's line, under the help
	// Under the action row, two lines at most.
	l.label("Help", 82, under, 990, 86, "$FontColorLightTeal", "left", true,
		"Click a cell and type, or a legend entry (Ctrl+L). Enter: done. Tab, arrows: next cell. Ctrl+V: paste. \"Name it\": the description.");
	for (int i = 0; i < kEditRows; ++i) {
		const size_t at = top + static_cast<size_t>(i);
		if (at >= e.fields.size()) {
			break;
		}
		const int          field  = e.fields[at];
		const bool         active = static_cast<int>(at) == e.field;
		const std::string& text   = field < 0 ? e.edit.description : e.edit.cell[static_cast<size_t>(field)];
		const int          y      = kEditTop + i * 78;
		l.label("Name" + std::to_string(i), 82, y, 290, 70, active ? "$FontColorWhite" : "$FontColorGoldYellow", "left", false, CellName(field));
		// the end of a long cell is what is being typed
		l.label("Value" + std::to_string(i), 380, y, 680, 70, active ? "$FontColorWhite" : "$FontColorLightGold", "left", true,
			(text.size() > 76 ? "..." + text.substr(text.size() - 73) : text) + (active ? "_" : text.empty() ? "-" : ""));
		l.spot("Cell" + std::to_string(i), 82, y, 980, 70, "Field" + std::to_string(at), "");
	}
	if (!e.problem.empty()) {
		l.label("Problem", 82, 1232, 990, 110, "$FontColorLightRed", "left", true, e.problem);
	}
	char text[120];
	snprintf(text, sizeof(text), "cells %zu - %zu of %zu", top + 1, top + kEditRows < e.fields.size() ? top + kEditRows : e.fields.size(), e.fields.size());
	l.bar(kEditTop, top, kEditRows, e.fields.size());
	l.pager(top > 0, top + kEditRows < e.fields.size(), text);
	return l.done();
}

// Asked when the book is left with changes not saved.
std::string PromptLayout(const Book& b, const std::string& panelName) {
	LayoutText l(b, panelName);
	l.title("Unsaved Changes");
	l.label("Ask", 82, 420, 990, 160, "$FontColorLightGold", "center", true,
		"You have " + std::to_string(g_editor.changes) + (g_editor.changes == 1 ? " change" : " changes")
			+ " that the cube does not have yet.\nSave writes them to recipes.txt and the cube uses them at once.");
	l.button("PromptSave", 331, 640, 500, "Save", "$FontColorGreen");
	l.button("PromptDiscard", 331, 720, 500, "Discard the changes", "$FontColorLightRed");
	l.button("PromptBack", 331, 800, 500, "Keep editing");
	return l.done();
}

// Asked before the list is changed in one go (search, restore, a template loaded) or a template deleted.
std::string ConfirmLayout(const Book& b, const std::string& panelName) {
	const Editor&     e     = g_editor;
	const size_t      count = e.confirmRows.size();
	const std::string many  = std::to_string(count) + (count == 1 ? " recipe" : " recipes");
	const std::string name  = e.templatePick >= 0 && static_cast<size_t>(e.templatePick) < e.templates.size() ? e.templates[static_cast<size_t>(e.templatePick)].name : std::string();
	LayoutText        l(b, panelName);
	std::string       text, yes;
	bool              unsaved = true; // the change waits for Save
	switch (e.asked) {
	case Asked::Search:
		l.title("Search for New Recipes");
		text = count == 0 ? "Every recipe of the mod and the game is in your list already (or was deleted by you). Nothing new was found."
		                  : many + " of the mod and the game " + (count == 1 ? "is" : "are") + " not in your list.\nAdd " + (count == 1 ? "it" : "them") + " at the end of your list?";
		yes  = "Add " + many;
		break;
	case Asked::Restore:
		l.title("Restore Deleted Defaults");
		text = count == 0 ? "No recipe of the mod or the game is noted as deleted by you. There is nothing to restore."
		                  : many + " of the mod and the game " + (count == 1 ? "was" : "were") + " deleted by you.\nPut " + (count == 1 ? "it" : "them") + " back at the end of your list?";
		yes  = "Put " + many + " back";
		break;
	case Asked::LoadTemplate:
		l.title("Load a Template");
		text = "Load the template \"" + name + "\" (" + many + ")?\nIt takes the place of the whole list in the book" + (e.changes != 0 ? ", changes not saved included" : "") + ".";
		yes  = "Load \"" + Fit(name, 24) + "\"";
		break;
	case Asked::RemoveRecipe: {
		const bool        valid = e.entry >= 0 && static_cast<size_t>(e.entry) < e.rows.size();
		const std::string about = valid && !e.rows[static_cast<size_t>(e.entry)].description.empty() ? e.rows[static_cast<size_t>(e.entry)].description : "(no description)";
		l.title("Remove a Recipe");
		text = "Remove this recipe from your list?\n\"" + Fit(about, 110) + "\""
		     + (valid && e.isDefault[static_cast<size_t>(e.entry)] != 0 ? "\nIt is a recipe of the mod or the game: \"Restore deleted defaults\" can bring it back." : "\nIt is a recipe of your own: once saved, it is gone.");
		yes  = "Remove it";
		break;
	}
	case Asked::DeleteTemplate:
		l.title("Delete a Template");
		text    = "Delete the template \"" + name + "\"?\nIts file is removed. The list in the book and the cube are not touched.";
		yes     = "Delete \"" + Fit(name, 22) + "\"";
		unsaved = false;
		break;
	}
	const bool nothing = (e.asked == Asked::Search || e.asked == Asked::Restore) && count == 0;
	l.label("Ask", 82, 400, 990, 200, "$FontColorLightGold", "center", true, text + (!nothing && unsaved ? "\n(Nothing reaches the cube before you save.)" : ""));
	if (nothing) {
		l.button("ConfirmNo", 331, 660, 500, "Back");
		return l.done();
	}
	l.button("ConfirmYes", 231, 660, 700, yes, e.asked == Asked::DeleteTemplate || e.asked == Asked::RemoveRecipe ? "$FontColorLightRed" : "$FontColorGreen");
	l.button("ConfirmNo", 331, 740, 500, "Cancel");
	if (e.asked != Asked::DeleteTemplate && e.asked != Asked::RemoveRecipe) {
		for (size_t i = 0; i < count && i < 6; ++i) { // the first few, to see what is meant
			l.label("First" + std::to_string(i), 82, 860 + static_cast<int>(i) * 44, 990, 40, "$FontColorGoldYellow", "left", false, Fit(e.confirmRows[i].description.empty() ? "(no description)" : e.confirmRows[i].description, 58));
		}
		if (count > 6) {
			l.label("More", 82, 860 + 6 * 44, 990, 40, "$FontColorGoldYellow", "left", false, "... and " + std::to_string(count - 6) + " more");
		}
	}
	return l.done();
}

constexpr size_t kTemplateRows = 9;

// The saved lists: keep the book's list under a name, load one back, delete one.
std::string TemplatesLayout(const Book& b, const std::string& panelName) {
	const Editor& e = g_editor;
	LayoutText    l(b, panelName);
	l.title("Recipe Templates");
	l.action("Back", "Back", "$FontColorWhite", 1);
	const int under = l.action("SaveAs", "Save this list as a template", "$FontColorGreen", 3);
	l.label("Help", 82, under, 990, 86, "$FontColorLightTeal", "left", true,
		"A template is a whole recipe list kept under a name. Loading one replaces the book's list; the cube has it once you save.");
	const int first = under + 92;
	if (e.templates.empty()) {
		l.label("None", 82, first + 20, 990, 40, "$FontColorGoldYellow", "center", false, "No template saved yet.");
	}
	for (size_t i = 0; i < e.templates.size() && i < kTemplateRows; ++i) {
		const int y = first + static_cast<int>(i) * 96;
		l.label("Name" + std::to_string(i), 82, y, 990, 40, "$FontColorLightGold", "left", false, Fit(e.templates[i].name, 44) + "   (" + std::to_string(e.templates[i].recipes) + " recipes)");
		l.button("Load" + std::to_string(i), 120, y + 42, 200, "Load");
		l.button("Delete" + std::to_string(i), 360, y + 42, 220, "Delete", "$FontColorLightRed");
	}
	if (e.templates.size() > kTemplateRows) {
		l.label("More", 82, first + static_cast<int>(kTemplateRows) * 96, 990, 40, "$FontColorGoldYellow", "left", false,
			"... and " + std::to_string(e.templates.size() - kTemplateRows) + " more in the templates folder");
	}
	return l.done();
}

// The name a new template is saved under.
std::string TemplateNameLayout(const Book& b, const std::string& panelName) {
	const Editor& e = g_editor;
	LayoutText    l(b, panelName);
	l.title("New Template");
	l.label("Ask", 82, 380, 990, 120, "$FontColorLightGold", "center", true,
		"Type a name for the template (letters, digits, spaces, - and _).\nThe list in the book, " + std::to_string(e.rows.size()) + " recipes, is saved under it. A template of the same name is replaced.");
	l.slot("NameSlot", 210, 520);
	l.label("NameTyped", 250, 548, 660, 56, "$FontColorWhite", "left", false, e.templateName + "_");
	l.button("NameDone", 231, 680, 330, "Save template", "$FontColorGreen");
	l.button("NameCancel", 601, 680, 330, "Cancel");
	if (!e.templateProblem.empty()) {
		l.label("Problem", 82, 760, 990, 80, "$FontColorLightRed", "center", true, e.templateProblem);
	}
	return l.done();
}

std::string BookLayout(const Book& b, const std::string& panelName) {
	if (b.legend || g_editor.view == View::List) {
		return ListLayout(b, panelName);
	}
	switch (g_editor.view) {
	case View::Card: return CardLayout(b, panelName);
	case View::Edit: return EditLayout(b, panelName);
	case View::Confirm: return ConfirmLayout(b, panelName);
	case View::Templates: return TemplatesLayout(b, panelName);
	case View::TemplateName: return TemplateNameLayout(b, panelName);
	default: return PromptLayout(b, panelName);
	}
}

bool AnyBookOpen() {
	return g_recipeBook.open.load() || g_legendBook.open.load();
}

// Is the book on screen (one of its tabs is the one its side shows)?
bool BookIsOpen(Book& b) {
	bool on = false;
	for (const int handle : b.handle) {
		on = on || (g_book != nullptr && handle != 0 && g_book->isShown(handle) != 0);
	}
	return on;
}

// ---- the editor's data

Bank* ShownBank() { // g_lock held: the first list (Reign of the Warlock's when the game has it)
	for (Bank& bank : g_banks) {
		if (bank.on) {
			return &bank;
		}
	}
	return nullptr;
}

std::string RowsText(const std::vector<cube::Cells>& rows, bool complete = true) {
	std::string text = (complete ? cube::CompleteHeader() : cube::FullHeader()) + "\r\n";
	for (const cube::Cells& row : rows) {
		text += cube::LineOf(row) + "\r\n";
	}
	return text;
}

std::vector<cube::Cells> SheetRows(const cube::Sheet& sheet) {
	std::vector<cube::Cells> rows;
	for (size_t i = 0; i < sheet.lines.size(); ++i) {
		if (!cube::IsBlank(sheet.lines[i])) {
			rows.push_back(cube::CellsOf(sheet, i));
			rows.back().remove      = false;
			rows.back().description = cube::ResultFirst(rows.back().description); // "C <- A + B" in the book, whatever the file has
		}
	}
	return rows;
}

// Called before a change: rows [at, at + before) are about to become `after` rows.
void EditorKeep(size_t at, size_t before, size_t after) {
	Editor&      e = g_editor;
	Editor::Step step;
	step.at      = at;
	step.rows.assign(e.rows.begin() + static_cast<ptrdiff_t>(at), e.rows.begin() + static_cast<ptrdiff_t>(at + before));
	step.count   = after;
	step.deleted = e.deleted;
	step.changes = e.changes;
	e.undo.push_back(std::move(step));
	if (e.undo.size() > 30) {
		e.undo.erase(e.undo.begin());
	}
	e.redo.clear();
}

// Takes the last change back (or, `again`, makes the last one taken back once more). False: there is none.
bool EditorUndo(bool again) {
	Editor&            e    = g_editor;
	std::vector<Editor::Step>& from = again ? e.redo : e.undo;
	if (from.empty() || from.back().at + from.back().count > e.rows.size()) {
		from.clear();
		return false;
	}
	Editor::Step step = std::move(from.back());
	from.pop_back();
	Editor::Step back;
	const auto   first = e.rows.begin() + static_cast<ptrdiff_t>(step.at);
	back.at      = step.at;
	back.rows.assign(first, first + static_cast<ptrdiff_t>(step.count));
	back.count   = step.rows.size();
	back.deleted = std::move(e.deleted);
	back.changes = e.changes;
	e.rows.erase(first, first + static_cast<ptrdiff_t>(step.count));
	e.rows.insert(e.rows.begin() + static_cast<ptrdiff_t>(step.at), step.rows.begin(), step.rows.end());
	e.deleted = std::move(step.deleted);
	e.changes = step.changes;
	(again ? e.undo : e.redo).push_back(std::move(back));
	return true;
}

// The user's list as the cube last took it becomes the editor's rows (changes not saved are
// dropped). A file that is still empty starts as the game's own list.
void EditorLoad() {
	Editor& e = g_editor;
	Guard   guard;
	e.rows.clear();
	e.deleted.clear();
	e.changes = 0;
	e.undo.clear();
	e.redo.clear();
	if (const Bank* bank = ShownBank()) {
		const cube::Sheet mine = cube::ReadSheet(std::string(bank->mineText));
		e.rows                 = SheetRows(mine.header.complete ? mine : cube::ReadSheet(std::string(bank->defaultsText)));
		if (e.rows.empty()) { // no recipes in the file: the game's own list
			e.rows = SheetRows(cube::ReadSheet(std::string(bank->defaultsText)));
		}
		e.deleted              = SheetRows(cube::ReadSheet(std::string(bank->deletedText)));
	}
}

bool EditorHasChanges() {
	return g_editor.changes != 0;
}

std::string EditorText() {
	return RowsText(g_editor.rows);
}

// The renames for the recipe book's rows (and the steps Undo would put back), on the UI thread.
void EditorRenameJob(const D2RL::PluginContext*, void*) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	std::vector<Renamed> renames;
	{
		Guard guard;
		renames.swap(g_editorRenames);
	}
	for (const Renamed& r : renames) {
		const bool stats = r.table == static_cast<int>(livetables::Table::ItemStatCost);
		int        cells = 0;
		for (cube::Cells& row : g_editor.rows) {
			cells += cube::RenameInCells(row, stats, r.oldName, r.newName);
		}
		for (cube::Cells& row : g_editor.deleted) { // the notes of deleted defaults, as deleted.txt
			cube::RenameInCells(row, stats, r.oldName, r.newName);
		}
		for (auto* steps : { &g_editor.undo, &g_editor.redo }) {
			for (Editor::Step& step : *steps) {
				for (cube::Cells& row : step.rows) {
					cube::RenameInCells(row, stats, r.oldName, r.newName);
				}
			}
		}
		if (cells != 0) {
			LogF("the recipe book: %s %s is now %s - %d %s of its rows rewritten%s", stats ? "stat" : "property", r.oldName, r.newName, cells, cells == 1 ? "cell" : "cells",
				g_editor.changes != 0 ? " (not saved yet)" : "");
		}
	}
	if (AnyBookOpen()) {
		BookAsk(BookJob);
	}
}

// The recipe book's entries: the editor's rows, one entry a row.
void EditorPreview(Book& b) {
	Editor& e = g_editor;
	Guard   guard;
	b.entries.clear();
	e.status.assign(e.rows.size(), cube::LineStatus::Blank);
	e.problemOf.assign(e.rows.size(), std::string());
	e.own.assign(e.rows.size(), 0);
	e.isDefault.assign(e.rows.size(), 0);
	Bank* bank = ShownBank();
	if (bank == nullptr) {
		b.found.clear();
		return;
	}
	RealGame          game(bank->version, bank->tables);
	const cube::Sheet mine = cube::ReadSheet(RowsText(e.rows));
	if (!bank->indexed) {
		bank->defaultsIndex = cube::IndexDefaults(game, bank->layout, cube::ReadSheet(std::string(bank->defaultsText)), bank->listCache);
		bank->indexed       = true;
	}
	// a line is worked out once (its entry, whether the game reads it, whether it is a default);
	// after a change only the changed line is new
	b.entries.reserve(e.rows.size());
	std::string key;
	for (size_t r = 0; r < e.rows.size() && r < mine.lines.size(); ++r) {
		key.assign(mine.lines[r]);
		auto held = bank->rowMemo.find(key);
		if (held != bank->rowMemo.end() && held->second.status == cube::LineStatus::Refused) {
			bank->rowMemo.erase(held); // worked out again: what it names may exist now (a property made in Live Properties)
			held = bank->rowMemo.end();
		}
		if (held == bank->rowMemo.end()) {
			Bank::RowMemo         memo;
			const cube::LineFacts facts = cube::FactsOf(game, bank->layout, bank->defaultsIndex, mine, r, bank->listCache);
			memo.status    = facts.status;
			memo.own       = facts.own;
			memo.isDefault = facts.isDefault;
			memo.shown     = cube::Summary(mine, r);
			if (memo.status == cube::LineStatus::Refused) {
				std::vector<uint8_t> row(cube::kWideRow);
				cube::CompileLine(game, bank->layout, mine.header, mine.lines[r], static_cast<int>(r), row.data(), memo.problem);
				memo.shown.title  = "[cannot be read] " + memo.shown.title;
				memo.shown.detail = memo.problem;
			}
			if (memo.status != cube::LineStatus::Refused) {
				const std::vector<std::string> notes = cube::AmountNotes(mine.header, mine.lines[r]);
				if (!notes.empty()) {
					memo.problem      = notes[0];
					memo.shown.title  = "[no amount] " + memo.shown.title;
					memo.shown.detail = notes[0] + ".   " + memo.shown.detail;
				}
			}
			if (memo.own) { // yours: marked, and found by searching "*"
				memo.shown.title  = "* " + memo.shown.title;
				memo.shown.search = "* " + memo.shown.search;
			}
			held = bank->rowMemo.emplace(key, std::move(memo)).first;
		}
		const Bank::RowMemo& memo = held->second;
		e.status[r]    = memo.status;
		e.own[r]       = memo.own ? 1 : 0;
		e.isDefault[r] = memo.isDefault ? 1 : 0;
		e.problemOf[r] = memo.problem;
		b.entries.push_back(memo.shown);
	}
	b.found = cube::Search(b.entries, b.words);
}

// ---- templates: <the plugin's folder>\templates\<name>.txt, each a whole list

std::wstring TemplatePath(const std::string& name) {
	return g_folder + L"\\templates\\" + Widen(name) + L".txt";
}

void TemplatesRead() {
	Editor& e = g_editor;
	e.templates.clear();
	WIN32_FIND_DATAW entry {};
	const HANDLE     find = FindFirstFileW((g_folder + L"\\templates\\*.txt").c_str(), &entry);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			continue;
		}
		const std::wstring file = entry.cFileName;
		std::string        text;
		Editor::Template   one;
		one.name = Narrow(file.substr(0, file.size() - 4));
		if (ReadWhole(g_folder + L"\\templates\\" + file, text)) { // counted, not read into rows
			const cube::Sheet sheet = cube::ReadSheet(std::move(text));
			for (const std::string_view line : sheet.lines) {
				one.recipes += cube::IsBlank(line) ? 0 : 1;
			}
		}
		e.templates.push_back(one);
	} while (FindNextFileW(find, &entry) != 0 && e.templates.size() < 200);
	FindClose(find);
	for (size_t i = 1; i < e.templates.size(); ++i) { // by name
		for (size_t j = i; j > 0 && _stricmp(e.templates[j].name.c_str(), e.templates[j - 1].name.c_str()) < 0; --j) {
			std::swap(e.templates[j], e.templates[j - 1]);
		}
	}
}

// Saves the book's list under the typed name. False: the name will not do (templateProblem says why).
bool TemplateSave() {
	Editor&     e    = g_editor;
	std::string name = e.templateName;
	while (!name.empty() && name.back() == ' ') {
		name.pop_back();
	}
	while (!name.empty() && name.front() == ' ') {
		name.erase(name.begin());
	}
	if (name.empty()) {
		e.templateProblem = "The template needs a name.";
		return false;
	}
	MakeFolders(g_folder + L"\\templates");
	if (!WriteOwn(TemplatePath(name), RowsText(e.rows))) {
		e.templateProblem = "The template could not be written (is its file open in another program?).";
		return false;
	}
	LogF("template \"%s\" saved: %zu recipes", name.c_str(), e.rows.size());
	e.templateProblem.clear();
	return true;
}

// The defaults the list lacks: those never in it (new) or those noted as deleted.
std::vector<cube::Cells> EditorMissing(bool deletedOnes) {
	Editor& e = g_editor;
	Guard   guard;
	std::vector<cube::Cells> rows;
	Bank*                    bank = ShownBank();
	if (bank == nullptr) {
		return rows;
	}
	RealGame             game(bank->version, bank->tables);
	const cube::Sheet    mine     = cube::ReadSheet(RowsText(e.rows));
	const cube::Sheet    defaults = cube::ReadSheet(std::string(bank->defaultsText));
	const cube::Sheet    notes    = cube::ReadSheet(RowsText(e.deleted, false));
	const cube::Compared against  = cube::Compare(game, bank->layout, defaults, mine, notes, bank->listCache);
	for (const size_t line : deletedOnes ? against.deleted : against.fresh) {
		rows.push_back(cube::CellsOf(defaults, line));
		rows.back().description = cube::ResultFirst(rows.back().description);
	}
	return rows;
}

enum class Shown { Yes, Later, No };

// What the book shows, read afresh when it changed. Called before a page is drawn.
void BookRefresh(Book& b) {
	if (b.stamp != g_bookStamp.load()) { // what the book shows changed (start, the recipes command, a save)
		b.stamp = g_bookStamp.load();
		if (b.legend) {
			Guard guard;
			b.entries.clear();
			if (const Bank* bank = ShownBank()) {
				b.entries = b.tab == 1 ? bank->legendValues : bank->legend;
			}
			b.found = cube::Search(b.entries, b.words);
		} else {
			if (g_editor.changes == 0) {
				EditorLoad(); // the file as the cube has it now
			}
			EditorPreview(b);
			if (g_editor.view == View::Card && (g_editor.entry < 0 || static_cast<size_t>(g_editor.entry) >= g_editor.rows.size())) {
				g_editor.view = View::List;
			}
		}
	}
	const size_t rows  = static_cast<size_t>(b.rows);
	if (b.top + rows > b.found.size()) {
		b.top = b.found.size() > rows ? b.found.size() - rows : 0;
	}
}

// Has the Mod Book draw the book's page again.
Shown BookShow(Book& b) {
	if (g_book != nullptr) {
		g_book->redraw(b.handle[b.legend ? b.tab : 0]);
	}
	return Shown::Yes;
}

void BookAsk(D2RL::Threads::Callback job) {
	if (g_threads != nullptr) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, job, nullptr));
	}
}

void BookClose(Book& b) {
	if (g_book != nullptr) {
		g_book->close(modbook::SideOf(g_book, b.handle[b.legend ? b.tab : 0], b.right ? modbook::kRight : modbook::kLeft)); // the player may have dragged the tab
	}
	b.open.store(false);
}

// ---- the editor's actions

// The cells in the order the editor shows them: what a recipe usually has first.
// Keeps the cell typed into among the cells shown.
void EditorSee() {
	Editor&   e    = g_editor;
	const int last = static_cast<int>(e.fields.size()) > kEditRows ? static_cast<int>(e.fields.size()) - kEditRows : 0;
	if (e.field < e.fieldTop) {
		e.fieldTop = e.field;
	} else if (e.field >= e.fieldTop + kEditRows) {
		e.fieldTop = e.field - kEditRows + 1;
	}
	e.fieldTop = e.fieldTop < 0 ? 0 : e.fieldTop > last ? last : e.fieldTop;
}

void EditorFields() {
	Editor&   e      = g_editor;
	const int active = e.field >= 0 && static_cast<size_t>(e.field) < e.fields.size() ? e.fields[static_cast<size_t>(e.field)] : -1; // stays the cell typed into
	std::vector<int> all = { -1 };
	const char* first[] = { "enabled", "numinputs", "input 1", "input 2", "input 3", "input 4", "input 5", "input 6", "input 7", "output", "lvl", "plvl", "ilvl" };
	std::vector<bool> taken(cube::kColumnCount, false);
	for (const char* name : first) {
		const int c = cube::ColumnIndex(name);
		all.push_back(c);
		taken[static_cast<size_t>(c)] = true;
	}
	const int output = cube::ColumnIndex("output");
	for (int c = output; c < static_cast<int>(cube::kColumnCount); ++c) { // mods, output b and c with theirs
		if (!taken[static_cast<size_t>(c)]) {
			all.push_back(c);
			taken[static_cast<size_t>(c)] = true;
		}
	}
	for (int c = 0; c < output; ++c) { // min diff, version, op, param, value, class, ladder seasons
		if (!taken[static_cast<size_t>(c)]) {
			all.push_back(c);
		}
	}
	// without "All cells": the cells nearly every recipe uses, and whatever this one has filled in
	std::vector<bool> usual(cube::kColumnCount, false);
	for (const char* name : { "enabled", "numinputs", "input 1", "input 2", "input 3", "output", "mod 1", "mod 1 min", "mod 1 max" }) {
		usual[static_cast<size_t>(cube::ColumnIndex(name))] = true;
	}
	e.fields.clear();
	e.field = 0;
	for (const int c : all) {
		if (e.allCells || c < 0 || usual[static_cast<size_t>(c)] || (static_cast<size_t>(c) < e.edit.cell.size() && !e.edit.cell[static_cast<size_t>(c)].empty())) {
			if (c == active) {
				e.field = static_cast<int>(e.fields.size());
			}
			e.fields.push_back(c);
		}
	}
	e.fieldTop = 0;
	EditorSee();
}

void EditorOpen(const cube::Cells& cells, int row) {
	Editor& e  = g_editor;
	e.edit     = cells;
	e.original = cells;
	e.edit.cell.resize(cube::kColumnCount);
	e.edit.remove = false;
	e.editRow     = row;
	e.allCells    = false;
	e.field       = 0;
	e.fields.clear();
	EditorFields();
	e.view = View::Edit;
}

// A description for a recipe from its cells: the result first, then what goes in, in the mod's names.
std::string EditorName(const cube::Cells& cells) {
	Guard       guard;
	const Bank* bank = ShownBank();
	return cube::GenericName(cells, bank != nullptr ? bank->names : cube::Names {});
}

// "Done" in the editor: the recipe has to be one the game can read; then it joins the rows.
bool EditorDone(Book& b) {
	Editor&     e  = g_editor;
	cube::Cells& c = e.edit;
	const int numinputs = cube::ColumnIndex("numinputs"), enabled = cube::ColumnIndex("enabled");
	if (c.cell[static_cast<size_t>(numinputs)].empty()) {
		c.cell[static_cast<size_t>(numinputs)] = std::to_string(cube::CountInputs(c)); // counted for you
	}
	if (c.cell[static_cast<size_t>(enabled)].empty()) {
		c.cell[static_cast<size_t>(enabled)] = "1";
	}
	if (c.cell[static_cast<size_t>(cube::ColumnIndex("input 1"))].empty() || c.cell[static_cast<size_t>(cube::ColumnIndex("output"))].empty()) {
		e.problem = "A recipe needs at least input 1 and output.";
		return false;
	}
	if (c.description.empty()) { // named for you: what comes out, then what goes in
		c.description = EditorName(c);
	}
	{
		Guard guard;
		Bank* bank = ShownBank();
		if (bank == nullptr) {
			e.problem = "The game's recipes are not read yet.";
			return false;
		}
		RealGame             game(bank->version, bank->tables);
		const cube::Sheet    one = cube::ReadSheet(RowsText({ c }));
		std::vector<uint8_t> row(cube::kWideRow);
		if (!cube::CompileLine(game, bank->layout, one.header, one.lines[0], 0, row.data(), e.problem)) {
			e.problem = "The game cannot read this: " + e.problem;
			return false;
		}
	}
	if (e.editRow >= 0) {
		const size_t row = static_cast<size_t>(e.editRow);
		EditorKeep(row, 1, 1);
		if (e.isDefault[row] != 0 && !cube::SameIngredientCells(e.original, c)) {
			e.deleted.push_back(e.original); // other ingredients: the default it was is gone from the list, and a search must not bring it back
		}
		e.rows[row] = c;
	} else {
		EditorKeep(0, 0, 1);
		e.rows.insert(e.rows.begin(), c); // a new recipe goes in front: the cube takes the first recipe that fits
	}
	++e.changes;
	e.problem.clear();
	e.view = View::List;
	EditorPreview(b);
	return true;
}

// Writes the list to recipes.txt (and the notes to deleted.txt) and has the cube take it, as
// the "recipes" command does.
void Ask();
void EditorSave(Book& b) {
	Editor&           e    = g_editor;
	const std::string text = RowsText(e.rows), notes = RowsText(e.deleted, false);
	bool              ok   = false;
	{
		Guard guard;
		if (Bank* bank = ShownBank()) {
			ok = WriteOwn(bank->path, text) && WriteOwn(bank->deletedPath, notes);
			if (ok) {
				bank->deletedText = notes;
				bank->pendingText = text;
				bank->pending     = true;
			}
		}
	}
	if (!ok) {
		g_ctx->WriteConsoleMessage("Live Cube Recipes: recipes.txt could not be written (is it open in a program that holds it?); your changes are still in the recipe book.");
		return;
	}
	e.changes = 0;
	e.undo.clear(); // what the cube has cannot be taken back
	e.redo.clear();
	e.view    = View::List;
	EditorPreview(b);
	Ask(); // the cube takes the file on the game's thread; the book shows the result when it has
}

// A click or Escape in the recipe book. True: something to show changed.
bool EditorClick(Book& b, const std::string& click) {
	Editor&    e    = g_editor;
	const auto leave = [&] { // after Save or Discard in the prompt
		if (e.closeAfterPrompt) {
			e.closeAfterPrompt = false;
			BookClose(b);
			return false;
		}
		return true;
	};
	if (click == "Escape") {
		if (e.view == View::Edit) {
			e.view = e.editRow >= 0 ? View::Card : View::List;
		} else if (e.view == View::Confirm && e.asked == Asked::RemoveRecipe) {
			e.view = View::Card;
		} else if (e.view == View::TemplateName || (e.view == View::Confirm && (e.asked == Asked::LoadTemplate || e.asked == Asked::DeleteTemplate))) {
			e.view = View::Templates;
		} else if (e.view == View::Card || e.view == View::Confirm || e.view == View::Templates) {
			e.view = View::List;
		} else if (e.view == View::Prompt) {
			e.view             = e.promptFrom;
			e.closeAfterPrompt = false;
		} else if (e.changes != 0) {
			e.promptFrom       = View::List;
			e.closeAfterPrompt = true;
			e.view             = View::Prompt;
		} else {
			BookClose(b);
			return false;
		}
		return true;
	}
	if (e.view == View::List) {
		if (click == "New") {
			e.entry = -1;
			cube::Cells blank;
			blank.cell.resize(cube::kColumnCount);
			blank.cell[static_cast<size_t>(cube::ColumnIndex("enabled"))] = "1";
			EditorOpen(blank, -1);
		} else if (click == "Save") {
			EditorSave(b);
		} else if (click == "Discard") {
			EditorLoad();
			EditorPreview(b);
		} else if (click == "Search" || click == "RestoreAll") {
			e.asked       = click == "RestoreAll" ? Asked::Restore : Asked::Search;
			e.confirmRows = EditorMissing(e.asked == Asked::Restore);
			e.view        = View::Confirm;
		} else if (click == "Undo" || click == "Redo") {
			if (!EditorUndo(click == "Redo")) {
				return false;
			}
			EditorPreview(b);
		} else if (click == "Templates") {
			TemplatesRead();
			e.view = View::Templates;
		} else if (click.rfind("Row", 0) == 0) {
			const size_t at = b.top + static_cast<size_t>(atoi(click.c_str() + 3));
			if (at >= b.found.size() || b.found[at] >= e.rows.size()) {
				return false;
			}
			e.entry = static_cast<int>(b.found[at]);
			e.view  = View::Card;
		} else {
			return false;
		}
		return true;
	}
	if (e.view == View::Confirm && e.asked == Asked::RemoveRecipe) {
		const bool valid = e.entry >= 0 && static_cast<size_t>(e.entry) < e.rows.size();
		if (click == "ConfirmYes" && valid) {
			const size_t row = static_cast<size_t>(e.entry);
			EditorKeep(row, 1, 0);
			if (e.isDefault[row] != 0) {
				e.deleted.push_back(e.rows[row]); // noted, so that a search for new recipes does not bring it back
			}
			e.rows.erase(e.rows.begin() + e.entry);
			++e.changes;
			e.view = View::List;
			EditorPreview(b);
			return true;
		}
		if (click == "ConfirmNo" || click == "ConfirmYes") {
			e.view = valid ? View::Card : View::List;
			return true;
		}
		return false;
	}
	if (e.view == View::Confirm) {
		const bool ofTemplate = e.asked == Asked::LoadTemplate || e.asked == Asked::DeleteTemplate;
		if (click == "ConfirmYes" && e.asked == Asked::DeleteTemplate && e.templatePick >= 0 && static_cast<size_t>(e.templatePick) < e.templates.size()) {
			const std::string name = e.templates[static_cast<size_t>(e.templatePick)].name;
			if (DeleteFileW(TemplatePath(name).c_str()) != 0) {
				LogF("template \"%s\" deleted", name.c_str());
			}
			TemplatesRead();
			e.view = View::Templates;
			return true;
		}
		if (click == "ConfirmYes" && e.asked == Asked::LoadTemplate) {
			EditorKeep(0, e.rows.size(), e.confirmRows.size());
			e.rows = e.confirmRows; // the template's list takes the place of the book's
			++e.changes;
			e.confirmRows.clear();
			e.view = View::List;
			b.top = 0;
			EditorPreview(b);
			return true;
		}
		if (click == "ConfirmYes" && !ofTemplate && !e.confirmRows.empty()) {
			EditorKeep(e.rows.size(), 0, e.confirmRows.size());
			e.rows.insert(e.rows.end(), e.confirmRows.begin(), e.confirmRows.end());
			if (e.asked == Asked::Restore) {
				e.deleted.clear(); // nothing is noted as deleted any more
			}
			++e.changes;
			e.confirmRows.clear();
			e.view = View::List;
			EditorPreview(b);
			return true;
		}
		if (click == "ConfirmNo") {
			e.confirmRows.clear();
			e.view = ofTemplate ? View::Templates : View::List;
			return true;
		}
		return false;
	}
	if (e.view == View::Templates) {
		if (click == "Back") {
			e.view = View::List;
		} else if (click == "SaveAs") {
			e.templateName.clear();
			e.templateProblem.clear();
			e.view = View::TemplateName;
		} else if (click.rfind("Load", 0) == 0 || click.rfind("Delete", 0) == 0) {
			const bool   load = click[0] == 'L';
			const size_t pick = static_cast<size_t>(atoi(click.c_str() + (load ? 4 : 6)));
			if (pick >= e.templates.size()) {
				return false;
			}
			e.templatePick = static_cast<int>(pick);
			e.asked        = load ? Asked::LoadTemplate : Asked::DeleteTemplate;
			e.confirmRows.clear();
			std::string text;
			if (load && ReadWhole(TemplatePath(e.templates[pick].name), text)) {
				e.confirmRows = SheetRows(cube::ReadSheet(std::move(text)));
			}
			e.view = View::Confirm;
		} else {
			return false;
		}
		return true;
	}
	if (e.view == View::TemplateName) {
		if (click == "NameDone") {
			if (TemplateSave()) {
				TemplatesRead();
				e.view = View::Templates;
			}
		} else if (click == "NameCancel") {
			e.view = View::Templates;
		} else {
			return false;
		}
		return true;
	}
	if (e.view == View::Prompt) {
		if (click == "PromptSave") {
			EditorSave(b);
			return leave();
		}
		if (click == "PromptDiscard") {
			EditorLoad();
			EditorPreview(b);
			e.view = View::List;
			return leave();
		}
		if (click == "PromptBack") {
			e.view             = e.promptFrom;
			e.closeAfterPrompt = false;
			return true;
		}
		return false;
	}
	if (e.view == View::Edit) {
		if (click == "Done") {
			EditorDone(b);
		} else if (click == "Name") {
			e.edit.description = EditorName(e.edit);
		} else if (click == "Cells") {
			e.allCells = !e.allCells;
			EditorFields();
		} else if (click == "Cancel") {
			return EditorClick(b, "Escape");
		} else if (click.rfind("Field", 0) == 0) {
			const int field = atoi(click.c_str() + 5);
			if (field >= 0 && static_cast<size_t>(field) < e.fields.size()) {
				e.field = field;
			}
		} else {
			return false;
		}
		return true;
	}
	// the card
	if (e.entry < 0 || static_cast<size_t>(e.entry) >= e.rows.size()) {
		e.view = View::List;
		return true;
	}
	const size_t      row   = static_cast<size_t>(e.entry);
	const cube::Cells cells = e.rows[row];
	if (click == "Back") {
		e.view = View::List;
		return true;
	}
	if (click == "Edit") {
		EditorOpen(cells, e.entry);
		return true;
	}
	if (click == "Copy") {
		cube::Cells copy = cells;
		copy.description += " (copy)";
		EditorOpen(copy, -1);
		return true;
	}
	if (click == "Toggle" && e.status[row] != cube::LineStatus::Refused) {
		EditorKeep(row, 1, 1);
		e.rows[row].cell[0] = cells.cell[0] == "0" ? "1" : "0";
	} else if (click == "Remove") {
		e.asked = Asked::RemoveRecipe; // asked first
		e.view  = View::Confirm;
		return true;
	} else {
		return false;
	}
	++e.changes;
	e.view = View::List;
	EditorPreview(b);
	return true;
}

// Typed keys in the recipe book's editor go to the cell picked.
// The clipboard's text as one cell: plain letters up to the first tab or line end, at most `room`.
std::string ClipboardText(size_t room) {
	std::string out;
	if (!OpenClipboard(nullptr)) {
		return out;
	}
	if (const HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
		if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data)); text != nullptr) {
			while (*text == L' ' || *text == L'\r' || *text == L'\n') {
				++text;
			}
			for (; *text != 0 && *text != L'\t' && *text != L'\r' && *text != L'\n' && out.size() < room; ++text) {
				if (*text >= 0x20 && *text < 0x7f) {
					out += static_cast<char>(*text);
				}
			}
			GlobalUnlock(data);
		}
	}
	CloseClipboard();
	return out;
}

void ClipboardSet(const std::string& text) {
	if (!OpenClipboard(nullptr)) {
		return;
	}
	EmptyClipboard();
	if (const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)); memory != nullptr) {
		auto* to = static_cast<wchar_t*>(GlobalLock(memory));
		for (size_t i = 0; i < text.size(); ++i) {
			to[i] = static_cast<unsigned char>(text[i]);
		}
		to[text.size()] = 0;
		GlobalUnlock(memory);
		if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
			GlobalFree(memory);
		}
	}
	CloseClipboard();
}

// Is a cell of the recipe editor being typed into (so that a legend entry can be put in)?
bool LegendCanInsert() {
	return g_recipeBook.open.load() && g_editor.view == View::Edit && !g_editor.fields.empty();
}

// A legend entry's Insert: its text (the entry's insert: "shld", "18") goes into the cell being
// typed into. False: no cell is, or the entry has nothing to put in.
bool LegendInsert(const std::string& code) {
	Editor& e = g_editor;
	if (!LegendCanInsert()) {
		return false;
	}
	if (code.empty()) {
		return false;
	}
	const int    field = e.fields[static_cast<size_t>(e.field)];
	std::string& text  = CellText(e.edit, field);
	if (field < 0) {
		text += (text.empty() ? "" : " ") + code;
	} else if (cube::kColumns[field].kind == cube::Kind::Input || cube::kColumns[field].kind == cube::Kind::Output) {
		const bool quoted = text.size() >= 2 && text.back() == '"'; // "shld,rar": added inside the quotes
		if (quoted) {
			text.pop_back();
		}
		text += (text.empty() || text.back() == ',' || text.back() == '=' || text.back() == '"' ? "" : ",") + code;
		if (quoted) {
			text += '"';
		}
	} else {
		text = code; // a number or one code: it takes the cell
	}
	e.problem.clear();
	EditorSee();
	return true;
}

bool EditorType(const std::string& keys) {
	Editor& e = g_editor;
	if (e.view == View::TemplateName && !keys.empty()) {
		for (const char c : keys) { // a file name: letters, digits, spaces, - and _
			if (c == '\b') {
				if (!e.templateName.empty()) {
					e.templateName.pop_back();
				}
			} else if (c == '\x7f') {
				e.templateName.clear();
			} else if (c == kKeyPaste) {
				for (const char p : ClipboardText(40)) {
					if (e.templateName.size() < 40 && ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || (p >= '0' && p <= '9') || p == ' ' || p == '-' || p == '_')) {
						e.templateName += p;
					}
				}
			} else if (e.templateName.size() < 40 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')) {
				e.templateName += c;
			}
		}
		e.templateProblem.clear();
		return true;
	}
	if (e.view != View::Edit || keys.empty()) {
		return false;
	}
	const int count = static_cast<int>(e.fields.size());
	for (const char c : keys) {
		std::string& text = CellText(e.edit, e.fields[static_cast<size_t>(e.field)]);
		if (c == '\b') {
			if (!text.empty()) {
				text.pop_back();
			}
		} else if (c == '\x7f') {
			text.clear();
		} else if (c == '\t' || c == kKeyDown) {
			e.field = (e.field + 1) % count;
		} else if (c == kKeyBack || c == kKeyUp) {
			e.field = (e.field + count - 1) % count;
		} else if (c == kKeyPaste) {
			text += ClipboardText(400 - (text.size() < 400 ? text.size() : 400));
		} else if (c == kKeyCopy) {
			ClipboardSet(text);
		} else if (static_cast<unsigned char>(c) >= 0x20 && text.size() < 400) {
			text += c;
		}
	}
	EditorSee();
	e.problem.clear();
	return true;
}

// Whatever was asked for since the last time: clicks, typed keys, paging, a changed list.
void __cdecl BookJob(const D2RL::PluginContext*, void*) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	bool again = false;
	for (Book* book : g_books) {
		Book& b = *book;
		AcquireSRWLockExclusive(&g_typeLock);
		const std::string              keys   = std::move(b.keys);
		const std::vector<std::string> clicks = std::move(b.clicks);
		b.keys.clear();
		b.clicks.clear();
		ReleaseSRWLockExclusive(&g_typeLock);
		if (g_book == nullptr || !BookIsOpen(b)) {
			b.open.store(false);
			b.move.store(0);
			b.jump.store(0);
			continue;
		}
		bool       dirty  = b.stamp != g_bookStamp.load() || b.redo.exchange(false);
		int        move   = b.move.exchange(0);
		int        scroll = b.scroll.exchange(0);
		const int  jump   = b.jump.exchange(0);
		bool       closed = false;
		for (const std::string& click : clicks) {
			if (click == "Previous" || click == "Next") {
				move += click == "Next" ? 1 : -1;
			} else if (b.legend) {
				if (click == "Escape") {
					BookClose(b);
					closed = true;
				} else if (click.rfind("Row", 0) == 0) {
					const size_t at = b.top + static_cast<size_t>(atoi(click.c_str() + 3));
					if (at < b.found.size() && LegendInsert(b.entries[b.found[at]].insert)) {
						g_recipeBook.redo.store(true); // the cell editor shows it
						again = true;
					}
				}
			} else {
				dirty = EditorClick(b, click) || dirty;
				closed = closed || !b.open.load();
			}
		}
		if (closed || !b.open.load()) {
			continue;
		}
		// the keys, in the order they were pressed; what a key does depends on what is shown by then
		std::string words = b.words;
		for (const char c : keys) {
			const View view = b.legend ? View::List : g_editor.view;
			if (view == View::Edit || view == View::TemplateName) {
				if (c == kKeyEnter || (c == kKeySave && view == View::Edit)) {
					dirty = EditorClick(b, view == View::Edit ? "Done" : "NameDone") || dirty;
					if (c == kKeySave && g_editor.view == View::List && g_editor.changes != 0) {
						dirty = EditorClick(b, "Save") || dirty;
					}
				} else {
					dirty = EditorType(std::string(1, c)) || dirty;
				}
			} else if (view == View::List) {
				if (c == '\b') {
					if (!words.empty()) {
						words.pop_back();
					}
				} else if (c == '\x7f') {
					words.clear();
				} else if (c == kKeyUp || c == kKeyDown) {
					scroll += c == kKeyDown ? 1 : -1;
				} else if (c == kKeyPaste) {
					for (const char p : ClipboardText(40 - (words.size() < 40 ? words.size() : 40))) {
						words += p >= 'A' && p <= 'Z' ? static_cast<char>(p + 32) : p;
					}
				} else if (!b.legend && (c == kKeyUndo || c == kKeyRedo)) {
					dirty = EditorClick(b, c == kKeyUndo ? "Undo" : "Redo") || dirty;
				} else if (!b.legend && c == kKeyNew) {
					dirty = EditorClick(b, "New") || dirty;
				} else if (!b.legend && c == kKeySave) {
					dirty = (g_editor.changes != 0 && EditorClick(b, "Save")) || dirty;
				} else if (static_cast<unsigned char>(c) >= 0x20 && words.size() < 40) { // the search: small letters
					words += c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
				}
			} else if (c == kKeyEnter) { // the button that says yes
				if (view == View::Confirm) {
					dirty = EditorClick(b, "ConfirmYes") || EditorClick(b, "ConfirmNo") || dirty;
				} else if (view == View::Prompt) {
					dirty = EditorClick(b, "PromptSave") || dirty;
				} else if (view == View::Card) {
					dirty = EditorClick(b, "Edit") || dirty;
				}
			}
			if (!b.open.load()) {
				break;
			}
		}
		if (!b.open.load()) {
			continue;
		}
		if (b.legend || g_editor.view == View::List) {
			if (words != b.words) {
				b.words = words;
				b.found = cube::Search(b.entries, b.words);
				b.top   = 0;
				dirty   = true;
			}
			const size_t rows = static_cast<size_t>(b.rows);
			const size_t last = b.found.size() > rows ? b.found.size() - rows : 0;
			const size_t was  = b.top;
			const long long to = jump < 0 ? 0 : jump > 0 ? static_cast<long long>(last) : static_cast<long long>(b.top) + static_cast<long long>(move) * b.rows + scroll;
			b.top = to < 0 ? 0 : static_cast<size_t>(to) > last ? last : static_cast<size_t>(to);
			dirty = dirty || b.top != was;
		} else if (g_editor.view == View::Edit && (move != 0 || scroll != 0 || jump != 0)) { // the cells: scrolled, the cell typed into stays
			Editor&   e    = g_editor;
			const int last = static_cast<int>(e.fields.size()) > kEditRows ? static_cast<int>(e.fields.size()) - kEditRows : 0;
			const int was  = e.fieldTop;
			const int to   = jump < 0 ? 0 : jump > 0 ? last : e.fieldTop + move * kEditRows + scroll;
			e.fieldTop     = to < 0 ? 0 : to > last ? last : to;
			dirty          = dirty || e.fieldTop != was;
		}
		if (!dirty) {
			continue; // nothing to show that is not shown (the hook thread asks four times a second)
		}
		static_cast<void>(BookShow(b));
	}
	if (again) {
		BookAsk(BookJob);
	}
}

// ---- the books as tabs of the Mod Book

struct TabRef {
	Book* book;
	int   tab; // the legend's: 0 words and codes, 1 columns and values
};
TabRef g_tabRefs[] = { { &g_recipeBook, 0 }, { &g_legendBook, 0 }, { &g_legendBook, 1 } };

void __cdecl TabDraw(void* user, modbook::Page* page) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	const TabRef& ref = *static_cast<TabRef*>(user);
	Book&         b   = *ref.book;
	if (b.legend && b.tab != ref.tab) { // the legend's two tabs dragged to different sides: each draws its own
		b.tab = ref.tab;
		b.top = 0;
	}
	g_page  = page;
	BookRefresh(b);
	static_cast<void>(BookLayout(b, std::string()));
	g_page = nullptr;
}

void __cdecl TabClick(void* user, const char* what) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	Book& b = *static_cast<TabRef*>(user)->book;
	AcquireSRWLockExclusive(&g_typeLock);
	b.clicks.emplace_back(what);
	ReleaseSRWLockExclusive(&g_typeLock);
	BookJob(g_ctx, nullptr);
}

void __cdecl TabKeys(void* user, const char* typed) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	Book& b = *static_cast<TabRef*>(user)->book;
	AcquireSRWLockExclusive(&g_typeLock);
	b.keys += typed;
	ReleaseSRWLockExclusive(&g_typeLock);
	BookJob(g_ctx, nullptr);
}

void __cdecl TabMove(void* user, int pages, int rows, int jump) noexcept {
	Book& b = *static_cast<TabRef*>(user)->book;
	b.move.fetch_add(pages);
	b.scroll.fetch_add(rows);
	if (jump != 0) {
		b.jump.store(jump);
	}
	BookJob(g_ctx, nullptr);
}

// The tab came on screen, or left it (its side was closed, or another tab is shown).
void __cdecl TabShown(void* user, int on) noexcept {
	g_uiThread.store(GetCurrentThreadId());
	const TabRef& ref = *static_cast<TabRef*>(user);
	Book&         b   = *ref.book;
	if (on == 0) {
		b.open.store(false);
		return;
	}
	if (b.legend && b.tab != ref.tab) { // the legend's other tab: its entries, from the top, the search kept
		b.tab = ref.tab;
		b.top = 0;
	}
	b.stamp = ~0ull; // read what it shows afresh
	if (!b.legend && g_editor.view == View::Prompt) {
		g_editor.view = View::List;
	}
	b.open.store(true);
}

// Asked before the book is closed or another tab is shown: with changes not saved, the recipe
// book shows its question instead.
int __cdecl TabLeave(void* user) noexcept {
	Book& b = *static_cast<TabRef*>(user)->book;
	if (!b.legend && g_editor.changes != 0 && g_editor.view != View::Prompt) {
		g_editor.promptFrom       = g_editor.view;
		g_editor.closeAfterPrompt = true;
		g_editor.view             = View::Prompt;
		return 0;
	}
	return 1;
}

// The Legend on the other side asks: is a cell being edited (it shows Insert then)?
int __cdecl TabEditing(void*) noexcept {
	return LegendCanInsert() ? 1 : 0;
}

// The Legend's Insert was clicked: its text goes into the cell being edited.
int __cdecl TabInsert(void*, const char* text) noexcept {
	if (text == nullptr || !LegendInsert(text)) {
		return 0;
	}
	g_recipeBook.redo.store(true); // the cell editor shows it
	BookJob(g_ctx, nullptr);
	return 1;
}

// ---- the legend as two sections of the Mod Book's Legend (Mod Book 0.8.0 and later)
//
// "Recipe codes" (the recipe words, item type, item and property codes) and "Recipe
// columns" (the columns, op conditions, stat numbers): the entries of legend.txt, read from the
// first list's bank whenever the Legend asks. Both follow the Cube Recipes tab.

int g_sections[2]     = { 0, 0 }; // their handles; 0: the Mod Book has no Legend (the old legend tabs are used)
int g_sectionUser[2]  = { 0, 1 };

int __cdecl SectionCount(void* user, int how) noexcept {
	const int section = *static_cast<int*>(user);
	if (how == 1) {
		return static_cast<int>(cube::LegendHow(section).size());
	}
	Guard       guard;
	const Bank* bank = ShownBank();
	return bank == nullptr ? 0 : static_cast<int>((section == 0 ? bank->legend : bank->legendValues).size());
}

int __cdecl SectionGet(void* user, int how, int index, modbook::LegendEntry* entry) noexcept {
	const int section = *static_cast<int*>(user);
	// the Mod Book copies them when get returns (UI thread only)
	static std::string code, meaning, detail, insert;
	if (entry == nullptr || index < 0) {
		return 0;
	}
	if (how == 1) {
		const std::vector<cube::HowItWorks> how = cube::LegendHow(section);
		if (static_cast<size_t>(index) >= how.size()) {
			return 0;
		}
		code    = how[static_cast<size_t>(index)].title;
		detail  = how[static_cast<size_t>(index)].text;
		meaning = detail.substr(0, detail.find(". ")); // its first sentence: one line
		meaning = Fit(meaning, 70);
		insert.clear();
	} else {
		Guard       guard;
		const Bank* bank = ShownBank();
		if (bank == nullptr) {
			return 0;
		}
		const std::vector<cube::Shown>& all = section == 0 ? bank->legend : bank->legendValues;
		if (static_cast<size_t>(index) >= all.size()) {
			return 0;
		}
		const cube::Shown& shown = all[static_cast<size_t>(index)];
		code    = shown.code;
		meaning = shown.meaning;
		detail  = shown.meaning.empty() ? shown.detail : shown.meaning + "\n" + shown.detail;
		insert  = shown.insert;
	}
	entry->code    = code.c_str();
	entry->meaning = meaning.c_str();
	entry->detail  = detail.c_str();
	entry->insert  = insert.empty() ? nullptr : insert.c_str();
	return 1;
}

// The legend's entries were made again (the tables were read): the Legend reads them afresh.
void SectionsChanged() {
	for (const int section : g_sections) {
		if (g_book != nullptr && section != 0) {
			g_book->legendChanged(section);
		}
	}
}

void BookToggle(Book& b) {
	BookSetUp();
	if (g_book == nullptr) {
		g_ctx->WriteConsoleMessage(("Live Cube Recipes: no panels - " + g_bookOff).c_str());
		return;
	}
	if (b.legend && g_sections[0] != 0) { // the Mod Book's Legend, on the recipe words (again: closed)
		g_book->showLegend(g_sections[0], nullptr);
		return;
	}
	g_book->toggle(b.handle[b.legend ? b.tab : 0]);
}

D2RL::ConsoleCommandResult __cdecl OnBookConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext*, void* which) noexcept {
	BookToggle(*static_cast<Book*>(which));
	return D2RL::ConsoleCommandResult::Handled;
}

// Puts the books into the Mod Book plugin as tabs: the recipes on the left, the legend's two
// lists on the right. Done when the game has read its tables (every plugin is loaded by then).
void BookSetUp() {
	static SRWLOCK once = SRWLOCK_INIT;
	AcquireSRWLockExclusive(&once);
	const struct Release {
		~Release() { ReleaseSRWLockExclusive(&once); }
	} release;
	if (g_book != nullptr) {
		return;
	}
	g_book = modbook::Find();
	if (g_book == nullptr) {
		g_bookOff = "the Mod Book plugin (d2rl-mod-book.dll) is not installed; recipes still work through recipes.txt and the recipes command";
		return;
	}
	// With the Mod Book's Legend (0.8.0 and later) the legend is two sections of it; an older Mod
	// Book gets the legend's two lists as tabs on the right, as before.
	const bool        legend   = modbook::HasLegend(g_book);
	const char* const ids[]    = { "recipes", "legendwords", "legendcolumns" };
	const char* const titles[] = { "Cube Recipes", "Recipe Words", "Recipe Terms" };
	const char* const groups[] = { nullptr, "Cube Recipes", "General" }; // the right side's groups; the left side has none
	bool              ok       = true;
	for (int i = 0; i < (legend ? 1 : 3); ++i) {
		const modbook::Tab tab { sizeof(modbook::Tab), i == 0 ? modbook::kLeft : modbook::kRight, ids[i], titles[i], &g_tabRefs[i], TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, groups[i],
			i == 0 ? TabEditing : nullptr, i == 0 ? TabInsert : nullptr };
		const int          handle = g_book->addTab(&tab);
		g_tabRefs[i].book->handle[g_tabRefs[i].tab] = handle;
		ok                                           = ok && handle != 0;
	}
	if (ok && legend) {
		const char* const sectionIds[]    = { "recipewords", "recipecolumns" };
		const char* const sectionTitles[] = { "Recipe codes", "Recipe columns" };
		for (int i = 0; i < 2; ++i) {
			const modbook::LegendSection section { sizeof(modbook::LegendSection), sectionIds[i], sectionTitles[i], g_recipeBook.handle[0], &g_sectionUser[i], SectionCount, SectionGet };
			g_sections[i] = g_book->addLegend(&section);
			ok            = ok && g_sections[i] != 0;
		}
	}
	if (!ok) {
		g_bookOff = "the Mod Book did not take the tabs (see mod-book.log)";
		g_book    = nullptr;
		return;
	}
	g_bookOff.clear();
	LogF(legend ? "panels: the recipe book is a tab of the Mod Book, the legend two sections of its Legend (Ctrl+B, Ctrl+L; console commands recipebook and recipelegend)"
	            : "panels: the recipe book and the legend are tabs of the Mod Book (Ctrl+B, Ctrl+L; console commands recipebook and recipelegend)");
}

// ---------------------------------------------------------------- console

// "recipes": reads the recipe files now and has the cube take them. This is the only time they
// are read after the game's start.
D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext*, void*) noexcept {
	std::vector<std::string> lines;
	bool                     ask = false;
	{
		Guard guard;
		if (!g_config.enabled) {
			lines.push_back("Live Cube Recipes: off in the settings (enabled = false).");
		} else if (!g_link.ok) {
			lines.push_back("Live Cube Recipes: off - " + g_linkProblem + ".");
		} else {
			ask = !g_setUp; // the tables-read notice did not come: the job looks at the tables itself
			for (Bank& bank : g_banks) {
				if (!bank.on) {
					lines.push_back(std::string("Live Cube Recipes: ") + bank.name + " is off: " + bank.off);
				} else if (ReadWhole(bank.path, bank.pendingText)) {
					bank.pending = true;
					ask          = true;
					lines.push_back("Live Cube Recipes: reading " + Narrow(bank.path));
				} else {
					bank.pendingText.clear();
					lines.push_back("Live Cube Recipes: " + Narrow(bank.path) + " could not be read (missing, or held by another program); the cube keeps the recipes it has.");
				}
			}
		}
	}
	for (const std::string& line : lines) {
		g_ctx->WriteConsoleMessage(line.c_str());
	}
	if (ask) {
		Ask();
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "live-cube-recipes",
	.name        = "Live Cube Recipes",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Your own Horadric Cube recipes in a file of the plugin's own, taken in while the game runs: add, change or remove a line and the cube uses it at once.",
	.flags       = D2RL::PluginFlags::Shared, // gameplay rules (the recipes) and UI (the recipe book)
	.reserved    = {},
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

// The sha256 of this DLL's file, as sha256sum prints it ("" when it cannot be read), so the log
// says which build ran (suite rule, lead-71 2026-11-25).
std::string OwnSha256(std::wstring& path) {
	HMODULE self = nullptr;
	wchar_t name[1024] = {};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnSha256), &self)
	    || GetModuleFileNameW(self, name, 1000) == 0) {
		return "";
	}
	path = name;
	std::string bytes;
	if (!ReadWhole(path, bytes)) {
		return "";
	}
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	uint8_t           digest[32] = {};
	bool              ok         = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
	ok = ok && BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) == 0;
	if (algorithm != nullptr) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	if (!ok) {
		return "";
	}
	char hex[65] = {};
	for (int i = 0; i < 32; ++i) {
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	}
	return hex;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) {
		return false;
	}
	g_ctx = context;
	{
		std::wstring      path;
		const std::string sha = OwnSha256(path);
		LogF("d2rl-live-cube-recipes.dll %s sha256 %s (%s)", kVersion, sha.empty() ? "unknown (the file could not be read)" : sha.c_str(), Narrow(path).c_str());
	}
	LoadConfig();
	static_cast<void>(context->RegisterConsoleCommand("recipes", OnConsole, "Live Cube Recipes: read your recipe file now and have the cube use it"));
	if (!g_config.enabled) {
		LogF("Live Cube Recipes %s: off in the settings (enabled = false)", kVersion);
		return true;
	}
	if (!LinkGame()) {
		LogF("Live Cube Recipes %s: off - %s. Recipes are as the game reads them.", kVersion, g_linkProblem.c_str());
		return true;
	}
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads     = nullptr;
		g_link.ok     = false;
		g_linkProblem = "this D2RLoader has no thread service";
		LogF("Live Cube Recipes %s: off - %s", kVersion, g_linkProblem.c_str());
		return true;
	}
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, registerGameplayEventListener))) {
		D2RL::Lifecycle::DataTablesLoadedListener tables {};
		tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
		tables.callback   = OnTablesLoaded;
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
	}
	BookSetUp(); // the Mod Book may load after this plugin: tried again when the tables are read
	static_cast<void>(context->RegisterConsoleCommand("recipebook", OnBookConsole, "Live Cube Recipes: open or close the recipe book (every recipe the cube has; type to search)", &g_recipeBook));
	static_cast<void>(context->RegisterConsoleCommand("recipelegend", OnBookConsole, "Live Cube Recipes: open or close the legend (what the words and codes in recipes mean; type to search)", &g_legendBook));
	// stay loaded: the game's recipe vector may point into memory this plugin fills
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Job), &self);
	LogF("Live Cube Recipes %s: on - your recipe files are read when the game reads its tables and when you type recipes in the console (D2R %s)", kVersion,
		context->buildVersion != nullptr ? context->buildVersion : "?");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_recipeBook.open.store(false);
	g_legendBook.open.store(false);
	// The game's list stays as it is: the memory it is in is never freed.
}
