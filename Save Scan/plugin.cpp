// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Save Scan: reads every saved item (all characters and the shared stash of the mod's save
// folder) and says which of them hold a stat. It exists for one question other plugins must
// ask before they change how a stat is saved: "would an item somewhere stop being readable?"
//
// It does NOTHING until asked: no hooks, no timers, nothing at load but its console command.
//   * other plugins: save_scan_api.h (savescan::Find, scan, dump)
//   * console (Ctrl + `):
//       savescan                 reads the files and says what it found
//       savescan <stat> [...]    the items that hold these stat numbers
//       savescan dump            writes d2rloader\save-scan\<mod>\items.txt (all items, all stats,
//                                and for each stat the properties that name it)
//
//   * savescan remove <stat>   a DRY RUN of fixSaves for "this middle row removed, every later stat
//                              one down": what would change in each file, into the plugin log
//
// scan and dump open saves for reading only. fixSaves (save_scan_api.h; fix_core.cpp) rewrites
// them on another plugin's request, after a dated backup, never while a character is in a game.
// The stat table is the one the game holds in memory at
// the moment of the call (so stats changed or made at run time are read as the game saves
// them); the item tables (armor, weapons, misc) are read from the mod's text files.
// The reader itself is save_core.cpp.

#include <D2RLPlugin/api.h>

#include "fix_core.h"
#include "save_core.h"
#include "save_scan_api.h"
#include "../Mod Book/mod_book_api.h"

#include <bcrypt.h>

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.3.4";

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

// ---------------------------------------------------------------- the game

constexpr uint32_t kKnownStamp = 0x6ab3782c; // the D2R image all addresses below are from

constexpr uintptr_t kTablesRva   = 0x300a90; // data tables of a data version (1 classic, 2 LoD, 3 RotW)
constexpr uintptr_t kStatsAt     = 0x1258;   // the ItemStatCost vector in the data tables
constexpr uintptr_t kStatNamesAt = 0x1270;   // the stat name table
constexpr uintptr_t kPropsAt     = 0x240;    // the Properties vector
constexpr uintptr_t kPropNamesAt = 0x270;    // the property code table
constexpr size_t    kPropRow     = 0x30, kPropFunc = 0x18, kPropStat = 0x20;

struct Vector {
	uint8_t* rows;
	uint64_t count;
	uint64_t capacity;
};

struct Print {
	uintptr_t   rva;
	const char* what;
	bool        entry; // the start of a function another plugin may have hooked (then it starts with a jump)
	uint8_t     size;
	uint8_t     bytes[16];
};
constexpr Print kPrints[] = {
	{ kTablesRva, "the data tables getter", true, 16, { 0x48, 0x83, 0xec, 0x28, 0x0f, 0xb6, 0xc1, 0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0x83, 0xf8, 0x04 } },
	{ 0x3e5dd0, "the stat name table's place", false, 7, { 0x48, 0x8b, 0x88, 0x70, 0x12, 0x00, 0x00 } },
	{ 0x31ecd5, "the stat table's place", false, 7, { 0x48, 0x8d, 0xbe, 0x58, 0x12, 0x00, 0x00 } },
	{ 0x31ed76, "the stat row size", false, 9, { 0x48, 0x69, 0x44, 0x24, 0x50, 0x44, 0x01, 0x00, 0x00 } },
	{ 0x32042c, "the property code table's place", false, 7, { 0x48, 0x8b, 0x88, 0x70, 0x02, 0x00, 0x00 } },
	{ 0x3d326d, "the property table's place", false, 7, { 0x48, 0x81, 0xc7, 0x40, 0x02, 0x00, 0x00 } },
};

bool Readable(const void* address, size_t size) {
	MEMORY_BASIC_INFORMATION info {};
	if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	return static_cast<const uint8_t*>(address) + size <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

uint32_t ImageStamp(uintptr_t base) {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (base == 0 || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xE00) {
		return 0;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

// Is this the game build the addresses are from, with its code as the plugin expects? Checked at each call.
bool GameKnown(uintptr_t& base, std::string& problem) {
	base                 = g_ctx->exeBase != 0 ? g_ctx->exeBase : reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const uint32_t stamp = ImageStamp(base);
	if (stamp != kKnownStamp) {
		char text[200];
		snprintf(text, sizeof(text), "this is another build of the game (image stamp %08x; the plugin knows %08x)", stamp, kKnownStamp);
		problem = text;
		return false;
	}
	for (const Print& print : kPrints) {
		const auto* at = reinterpret_cast<const uint8_t*>(base + print.rva);
		if (memcmp(at, print.bytes, print.size) != 0 && !(print.entry && (at[0] == 0xe9 || (at[0] == 0xff && at[1] == 0x25)))) {
			problem = std::string("the game's code is not what the plugin expects at ") + print.what;
			return false;
		}
	}
	return true;
}

// The names in one of the game's name tables (bucket count at +0x18, buckets at +0x20; an
// entry is {next, text, length, ..., number at +0x30}) - as Live Properties reads them.
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

std::wstring LoaderFolder() {
	std::wstring loader;
	if (g_ctx->pluginDirectory != nullptr && g_ctx->pluginDirectory[0] != L'\0') {
		loader = g_ctx->pluginDirectory;
		while (!loader.empty() && (loader.back() == L'\\' || loader.back() == L'/')) {
			loader.pop_back();
		}
		const size_t slash = loader.find_last_of(L"\\/");
		loader             = slash != std::wstring::npos && _wcsicmp(loader.c_str() + slash + 1, L"plugins") == 0 ? loader.substr(0, slash) : std::wstring();
	}
	return loader.empty() ? GameDirectory() + L"\\d2rloader" : loader;
}

std::string ModName() {
	return g_ctx->activeMod != nullptr ? g_ctx->activeMod : "";
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

// <loader folder>\live-suite\save-scan\<mod>: beside the plugins folder. Never inside a mod's .mpq.
std::wstring OwnFolder() {
	return SuiteFolder(LoaderFolder(), L"save-scan", Widen(ModName().empty() ? "no mod" : ModName()), g_moved);
}

void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

// The whole file, read only, sharing it with whoever else has it open.
bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	text.clear();
	char  buffer[65536];
	DWORD got = 0;
	bool  ok  = true;
	while ((ok = ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != 0) && got != 0) {
		text.append(buffer, got);
		if (text.size() > 64u * 1024 * 1024) {
			ok = false;
			break;
		}
	}
	CloseHandle(file);
	return ok;
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

// Files under a loader folder that end in `tail` (other plugins keep changed copies of the
// mod's tables there, Exalted Items for one). Read only.
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
			if (depth > 0 && name != L"." && name != L".." && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && _wcsicmp(name.c_str(), L"logs") != 0 && _wcsicmp(name.c_str(), L"logs-saved") != 0
				&& _wcsicmp(name.c_str(), L"plugins") != 0 && _wcsicmp(name.c_str(), L"crashes") != 0 && _wcsicmp(name.c_str(), L"save-scan") != 0) {
				FindCopies(path, tail, depth - 1, found);
			}
		} else if (path.size() >= tail.size() && _wcsicmp(path.c_str() + path.size() - tail.size(), tail.c_str()) == 0 && found.size() < 16) {
			found.push_back(path);
		}
	} while (FindNextFileW(find, &entry) != 0);
	FindClose(find);
}

// The folder the game reads the active mod's loose files from: mods/<mod>/<mod>.mpq/data.
bool FindDataRoot(std::wstring& root) {
	const std::string modName = ModName();
	if (modName.empty()) {
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
	return false;
}

// The save folder the game uses: Saved Games\Diablo II Resurrected, and mods\<mod> in it with a mod.
std::wstring SaveFolder() {
	wchar_t      profile[1024] = {};
	const DWORD  length        = GetEnvironmentVariableW(L"USERPROFILE", profile, 1000);
	std::wstring folder        = std::wstring(profile, length < 1000 ? length : 0) + L"\\Saved Games\\Diablo II Resurrected";
	if (!ModName().empty()) {
		folder += L"\\mods\\" + Widen(ModName());
	}
	return folder;
}

// ---------------------------------------------------------------- the scan

struct Looked { // one file
	std::string          name;
	savescan::FileResult result;
	int                  version = 0; // the data version whose stat table read it
};

struct Scan {
	bool                     ran = false;
	std::string              problem; // why it could not run
	std::vector<Looked>      files;
	std::vector<std::string> statNames, propNames;
	std::vector<uint8_t>     props; // the property rows (data version 3, or the newest there is)
	size_t                   propCount = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT; // one scan at a time

// The stat tables the game holds, newest data version first. Each is checked: a few stats whose
// saved shape every version of the game has must read as they should - else the row layout is
// not the one the plugin knows, and nothing is read with it.
bool GameTables(uintptr_t base, savescan::Tables shapes[3], int versions[3], int& count, Scan& scan, std::string& problem, bool names = true) {
	const auto getter = reinterpret_cast<void* (*)(uint8_t)>(base + kTablesRva);
	count             = 0;
	for (int version = 3; version >= 1; --version) {
		auto* tables = static_cast<uint8_t*>(getter(static_cast<uint8_t>(version)));
		if (!Readable(tables, kStatsAt + sizeof(Vector))) {
			continue;
		}
		const auto* stats = reinterpret_cast<const Vector*>(tables + kStatsAt);
		if (stats->count < 360 || stats->count > 0x8000 || !Readable(stats->rows, stats->count * savescan::kStatRow)) {
			continue;
		}
		savescan::Tables& out = shapes[count];
		savescan::ShapesFromRows(out, stats->rows, stats->count);
		// item_nonclassskill 6 bits + 9 param, item_addskill_tab 3 + 16, item_charged_skill 16 + 16, armorclass 11 bits add 10
		const auto is = [&](size_t stat, int bits, int param, int add) { return out.stats[stat].saveBits == bits && out.stats[stat].paramBits == param && out.stats[stat].saveAdd == add; };
		if (!is(97, 6, 9, 0) || !is(188, 3, 16, 0) || !is(204, 16, 16, 0) || !is(31, 11, 0, 10)) {
			char text[240];
			snprintf(text, sizeof(text), "data version %d: stats 97, 188, 204 and 31 do not have the saved shape every game table has (97 reads %d bits, %d param bits), so the row layout is not the one the plugin knows",
				version, out.stats[97].saveBits, out.stats[97].paramBits);
			problem = text;
			return false;
		}
		if (count == 0 && names) { // names and properties for the dump, from the newest
			WalkNames(tables, kStatNamesAt, stats->count, scan.statNames);
			const auto* props = reinterpret_cast<const Vector*>(tables + kPropsAt);
			if (Readable(props, sizeof(Vector)) && props->count > 0 && props->count < 0x7000 && Readable(props->rows, props->count * kPropRow)) {
				scan.props.assign(props->rows, props->rows + props->count * kPropRow);
				scan.propCount = props->count;
				WalkNames(tables, kPropNamesAt, props->count, scan.propNames);
			}
		}
		versions[count++] = version;
	}
	if (count == 0) {
		problem = "the game has not read its tables yet";
		return false;
	}
	return true;
}

bool ItemTables(savescan::Tables& tables, std::string& problem) {
	static const struct {
		const wchar_t* file;
		uint8_t        kind;
	} kFiles[] = { { L"armor.txt", savescan::kArmor }, { L"weapons.txt", savescan::kWeapon }, { L"misc.txt", savescan::kMisc } };
	std::wstring root;
	const bool   hasRoot = FindDataRoot(root);
	for (const auto& entry : kFiles) {
		const std::wstring        tail = std::wstring(L"\\global\\excel\\") + entry.file;
		std::vector<std::wstring> paths;
		if (hasRoot) {
			paths.push_back(root + tail);
		}
		FindCopies(LoaderFolder(), tail, 8, paths);
		int read = 0;
		for (const std::wstring& path : paths) {
			std::string text;
			if (ReadWhole(path, text) && !text.empty()) {
				savescan::ReadItemTable(tables, text, entry.kind);
				++read;
			}
		}
		if (read == 0) {
			problem = "no " + Narrow(entry.file) + " was found (the mod's loose data folder, or a plugin's copy under d2rloader): without the item tables an item's shape is not known";
			return false;
		}
	}
	return true;
}

Scan Run() {
	Scan        scan;
	uintptr_t   base = 0;
	if (!GameKnown(base, scan.problem)) {
		return scan;
	}
	savescan::Tables shapes[3];
	int              versions[3] = {}, count = 0;
	if (!GameTables(base, shapes, versions, count, scan, scan.problem)) {
		return scan;
	}
	if (!ItemTables(shapes[0], scan.problem)) {
		return scan;
	}
	for (int i = 1; i < count; ++i) {
		shapes[i].items = shapes[0].items;
	}
	const std::wstring folder = SaveFolder();
	if (!IsDirectory(folder)) {
		scan.problem = "the save folder was not found: " + Narrow(folder);
		return scan;
	}
	for (const wchar_t* pattern : { L"\\*.d2s", L"\\*.d2i" }) {
		WIN32_FIND_DATAW entry {};
		const HANDLE     find = FindFirstFileW((folder + pattern).c_str(), &entry);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
				continue;
			}
			Looked looked;
			looked.name = Narrow(entry.cFileName);
			std::string bytes;
			if (!ReadWhole(folder + L"\\" + entry.cFileName, bytes)) {
				looked.result.problem = "the file could not be opened";
			} else {
				const auto* data  = reinterpret_cast<const uint8_t*>(bytes.data());
				const bool  stash = pattern[3] == L'd' && pattern[5] == L'i';
				for (int i = 0; i < count; ++i) { // a classic or LoD character is saved with that data version's table
					savescan::FileResult result = stash ? savescan::ReadStash(data, bytes.size(), shapes[i]) : savescan::ReadCharacter(data, bytes.size(), shapes[i]);
					if (result.ok || i == 0) {
						const bool ok  = result.ok;
						looked.result  = std::move(result);
						looked.version = versions[i];
						if (ok) {
							break;
						}
					}
				}
			}
			scan.files.push_back(std::move(looked));
		} while (FindNextFileW(find, &entry) != 0);
		FindClose(find);
	}
	scan.ran = true;
	return scan;
}

void Count(const Scan& scan, savescan::Summary& summary) {
	summary.ran   = scan.ran ? 1 : 0;
	summary.files = static_cast<uint32_t>(scan.files.size());
	summary.unreadable = summary.items = summary.holders = 0;
	for (const Looked& file : scan.files) {
		summary.unreadable += file.result.ok ? 0 : 1;
		summary.items      += static_cast<uint32_t>(file.result.items.size());
	}
}

void Problems(const Scan& scan, savescan::ProblemFn problem, void* user) {
	if (problem == nullptr) {
		return;
	}
	if (!scan.ran) {
		problem(user, "", scan.problem.c_str());
	}
	for (const Looked& file : scan.files) {
		if (!file.result.ok) {
			problem(user, file.name.c_str(), file.result.problem.c_str());
		}
	}
}

int __cdecl ApiScan(const uint16_t* stats, uint32_t count, savescan::Summary* summary, savescan::HitFn hit, savescan::ProblemFn problem, void* user) noexcept {
	if (g_ctx == nullptr || summary == nullptr || summary->size < sizeof(savescan::Summary) || (count != 0 && stats == nullptr)) {
		return 0;
	}
	AcquireSRWLockExclusive(&g_lock);
	const Scan scan = Run();
	Count(scan, *summary);
	const std::set<uint16_t> wanted(stats, stats + count);
	for (const Looked& file : scan.files) {
		// An unreadable file's items are reported too, as far as they were read; the file still counts as unreadable.
		for (const savescan::Item& item : file.result.items) {
			std::set<uint16_t> seen;
			for (const savescan::StatValue& value : item.stats) {
				if (wanted.count(value.stat) == 0 || !seen.insert(value.stat).second) {
					continue;
				}
				if (hit != nullptr) {
					const std::string   where = savescan::Where(item);
					const savescan::Hit one { sizeof(savescan::Hit), file.name.c_str(), where.c_str(), item.code.c_str(), value.stat, value.value, value.param, item.offset };
					hit(user, &one);
				}
			}
			summary->holders += seen.empty() ? 0 : 1;
		}
	}
	Problems(scan, problem, user);
	LogF("scan for %u stat%s: %s, %u files (%u unreadable), %u items, %u hold one", count, count == 1 ? "" : "s", scan.ran ? "done" : ("could not run - " + scan.problem).c_str(), summary->files, summary->unreadable,
		summary->items, summary->holders);
	for (const Looked& file : scan.files) {
		if (!file.result.ok) {
			LogF("  unreadable: %s - %s", file.name.c_str(), file.result.problem.c_str());
		}
	}
	ReleaseSRWLockExclusive(&g_lock);
	return static_cast<int>(summary->ran);
}

int __cdecl ApiScanAffixes(const savescan::AffixId* ids, uint32_t count, savescan::Summary* summary, savescan::AffixHitFn hit, savescan::ProblemFn problem, void* user) noexcept {
	if (g_ctx == nullptr || summary == nullptr || summary->size < sizeof(savescan::Summary) || (count != 0 && ids == nullptr)) {
		return 0;
	}
	AcquireSRWLockExclusive(&g_lock);
	const Scan scan = Run();
	Count(scan, *summary);
	std::set<uint32_t> wanted;
	for (uint32_t i = 0; i < count; ++i) {
		wanted.insert(static_cast<uint32_t>(ids[i].suffix & 1) << 16 | ids[i].id);
	}
	for (const Looked& file : scan.files) {
		for (const savescan::Item& item : file.result.items) {
			bool holds = false;
			for (const savescan::AffixSlot& slot : item.affixes) {
				if (slot.id == 0 || wanted.count(static_cast<uint32_t>(slot.suffix) << 16 | slot.id) == 0) {
					continue;
				}
				holds = true;
				if (hit != nullptr) {
					const std::string        where = savescan::Where(item);
					const savescan::AffixHit one { sizeof(savescan::AffixHit), file.name.c_str(), where.c_str(), item.code.c_str(), slot.suffix, slot.slot, item.quality, slot.id, item.offset };
					hit(user, &one);
				}
			}
			summary->holders += holds ? 1 : 0;
		}
	}
	Problems(scan, problem, user);
	LogF("affix scan for %u id%s: %s, %u files (%u unreadable), %u items, %u carry one", count, count == 1 ? "" : "s", scan.ran ? "done" : ("could not run - " + scan.problem).c_str(), summary->files, summary->unreadable,
		summary->items, summary->holders);
	ReleaseSRWLockExclusive(&g_lock);
	return static_cast<int>(summary->ran);
}

const char* const kQualities[] = { "", "low", "normal", "superior", "magic", "set", "rare", "unique", "crafted" };

std::string DumpText(const Scan& scan) {
	const auto statName = [&](uint16_t stat) { return stat < scan.statNames.size() && !scan.statNames[stat].empty() ? scan.statNames[stat] : std::string("(no name)"); };
	std::string out = "SAVE SCAN - every saved item and its stats, read with the stat table the game holds now.\r\n"
	                  "Written when asked (console: savescan dump). The saves were only read.\r\n\r\n";
	std::map<uint16_t, std::pair<int, std::set<std::string>>> use; // stat -> items, files
	char                                                      line[400];
	for (const Looked& file : scan.files) {
		snprintf(line, sizeof(line), "== %s: %zu items%s\r\n", file.name.c_str(), file.result.items.size(), file.result.ok ? "" : "  ** NOT READ TO ITS END - what follows is only the part that was read **");
		out += line;
		if (!file.result.ok) {
			out += "   problem: " + file.result.problem + "\r\n";
		}
		for (const savescan::Item& item : file.result.items) {
			snprintf(line, sizeof(line), "  %-4s %-28s %s%s%s\r\n", item.code.c_str(), savescan::Where(item).c_str(), item.quality < 9 ? kQualities[item.quality] : "?",
				item.level != 0 ? (", level " + std::to_string(item.level)).c_str() : "", item.quantity != 0 ? (", quantity " + std::to_string(item.quantity)).c_str() : "");
			out += line;
			std::set<uint16_t> seen;
			for (const savescan::StatValue& value : item.stats) {
				snprintf(line, sizeof(line), "         %3u %-34s %d%s\r\n", value.stat, statName(value.stat).c_str(), value.value, value.param != 0 ? ("   (param " + std::to_string(value.param) + ")").c_str() : "");
				out += line;
				if (seen.insert(value.stat).second) {
					++use[value.stat].first;
					use[value.stat].second.insert(file.name);
				}
			}
		}
		out += "\r\n";
	}
	out += "== STATS ON SAVED ITEMS, and the properties that name each stat\r\n\r\n";
	for (const auto& [stat, where] : use) {
		snprintf(line, sizeof(line), "%3u %-34s %d item%s in %zu file%s\r\n", stat, statName(stat).c_str(), where.first, where.first == 1 ? "" : "s", where.second.size(), where.second.size() == 1 ? "" : "s");
		out += line;
		std::string files = "      files:";
		for (const std::string& name : where.second) {
			files += " " + name;
		}
		out += files + "\r\n";
		std::string props;
		for (size_t row = 0; row < scan.propCount; ++row) {
			const uint8_t* at = scan.props.data() + row * kPropRow;
			for (int i = 0; i < 7; ++i) {
				uint16_t named;
				memcpy(&named, at + kPropStat + i * 2, 2);
				if (at[kPropFunc + i] != 0 && named == stat) {
					props += " " + (row < scan.propNames.size() && !scan.propNames[row].empty() ? scan.propNames[row] : "#" + std::to_string(row));
					break;
				}
			}
		}
		out += "      properties:" + (props.empty() ? std::string(" (none names it)") : props) + "\r\n";
	}
	return out;
}

int __cdecl ApiDump(char* path, uint32_t pathSize, savescan::Summary* summary, savescan::ProblemFn problem, void* user) noexcept {
	if (g_ctx == nullptr) {
		return 0;
	}
	AcquireSRWLockExclusive(&g_lock);
	const Scan        scan = Run();
	savescan::Summary counted { sizeof(counted) };
	Count(scan, counted);
	if (summary != nullptr && summary->size >= sizeof(savescan::Summary)) {
		*summary = counted;
	}
	Problems(scan, problem, user);
	int done = 0;
	if (scan.ran) {
		const std::wstring folder = OwnFolder(), file = folder + L"\\items.txt";
		if (!g_moved.empty()) {
			LogF("its files were %s", g_moved.c_str());
			g_moved.clear();
		}
		MakeFolders(folder);
		done = WriteOwn(file, DumpText(scan)) ? 1 : 0;
		if (path != nullptr && pathSize != 0) {
			snprintf(path, pathSize, "%s", Narrow(file).c_str());
		}
		if (done == 0 && problem != nullptr) {
			problem(user, "", ("could not write " + Narrow(file)).c_str());
		}
		LogF("dump: %s (%u files, %u unreadable, %u items)", done != 0 ? Narrow(file).c_str() : "could not be written", counted.files, counted.unreadable, counted.items);
	} else {
		LogF("dump: could not run - %s", scan.problem.c_str());
	}
	ReleaseSRWLockExclusive(&g_lock);
	return done;
}


// ---------------------------------------------------------------- the save writer (fixSaves)

// Is a character in a game? D2RCore's IsInGame. -1: cannot tell.
int InGame() {
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	const auto    fn   = core != nullptr ? reinterpret_cast<bool(__cdecl*)()>(reinterpret_cast<void*>(GetProcAddress(core, "IsInGame"))) : nullptr;
	return fn == nullptr ? -1 : fn() ? 1 : 0;
}

std::wstring Stamp() {
	wchar_t    text[32];
	const time_t now = time(nullptr);
	tm         local {};
	localtime_s(&local, &now);
	wcsftime(text, 32, L"%Y%m%d-%H%M%S", &local);
	return text;
}

// The disk, for fix_core: the save folder and Save Scan's own folder (for the backup).
struct WinIo : savescan::FixIo {
	std::wstring saves, own, backupFolder;
	bool backup(const std::vector<savescan::FixInput>& files, std::string& folder, std::string& why) override {
		MakeFolders(own);
		backupFolder = own + L"\\backup-" + Stamp();
		if (GetFileAttributesW(backupFolder.c_str()) != INVALID_FILE_ATTRIBUTES || !CreateDirectoryW(backupFolder.c_str(), nullptr)) {
			why = "the backup folder " + Narrow(backupFolder) + " could not be made new";
			return false;
		}
		std::string list = "Save Scan backup, made before rewriting saves. Each file here came from:\r\n";
		for (const savescan::FixInput& file : files) {
			const std::wstring copy = backupFolder + L"\\" + Widen(file.name);
			std::string        back;
			if (!WriteOwn(copy, file.bytes) || !ReadWhole(copy, back) || back != file.bytes) {
				why = "the backup of " + file.name + " could not be written and read back";
				return false;
			}
			list += file.name + "\t" + Narrow(saves) + "\\" + file.name + "\r\n";
		}
		WriteOwn(backupFolder + L"\\where-each-file-came-from.txt", list);
		folder = Narrow(backupFolder);
		return true;
	}
	bool write(const std::string& name, const std::string& bytes, std::string& why) override {
		const std::wstring path = saves + L"\\" + Widen(name), temp = path + L".save-scan-writing";
		const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			why = "error " + std::to_string(GetLastError()) + " making " + Narrow(temp);
			return false;
		}
		DWORD      written = 0;
		const bool ok      = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
		CloseHandle(file);
		if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			why = "error " + std::to_string(GetLastError()) + " writing " + name;
			DeleteFileW(temp.c_str());
			return false;
		}
		return true;
	}
	bool read(const std::string& name, std::string& bytes) override { return ReadWhole(saves + L"\\" + Widen(name), bytes); }
	bool mayWrite(std::string& why) override {
		const int inGame = InGame();
		if (inGame != 0) {
			why = inGame > 0 ? "a character went into a game (the game holds that character and its stash)" : "it cannot be told whether a character is in a game";
			return false;
		}
		return true;
	}
	bool planned(const std::vector<savescan::RestoreEntry>& plan, std::string& why) override {
		std::string text = "# Save Scan: the files this fix writes (name, size and FNV-1a hash of what is written). restore() puts back only these.\r\n";
		for (const savescan::RestoreEntry& entry : plan) {
			char line[400];
			snprintf(line, sizeof(line), "%s\t%llu\t%016llx\r\n", entry.name.c_str(), static_cast<unsigned long long>(entry.writtenSize), static_cast<unsigned long long>(entry.writtenHash));
			text += line;
		}
		std::string back;
		if (!WriteOwn(backupFolder + L"\\written.txt", text) || !ReadWhole(backupFolder + L"\\written.txt", back) || back != text) {
			why = "written.txt could not be written into " + Narrow(backupFolder);
			return false;
		}
		return true;
	}
};

struct FixJob {
	savescan::FixRequest              request {};
	std::vector<savescan::ChangeSpec> changes;
	std::vector<savescan::ResizeSpec> resizes, before;
	std::vector<savescan::AffixSpec>  affixes;
	int64_t                           notAfter = 0;
	savescan::Tables                  shapes[3];
	int                               versions[3] = {}, count = 0;
	std::string                       refusal; // refused at the call: said in done() only
	std::wstring                      saves, own;
	savescan::FixDone*                now = nullptr; // fixSavesNow / restore: the caller's answer
};

volatile LONG g_fixBusy   = 0;
HANDLE        g_fixThread = nullptr;
std::string   g_nowBackup, g_nowWhy; // the strings of the last fixSavesNow / restore answer

const char* StateName(savescan::FixState state) {
	switch (state) {
	case savescan::FixState::Unchanged: return "unchanged";
	case savescan::FixState::Written: return "WRITTEN and checked";
	case savescan::FixState::WouldWrite: return "would be rewritten";
	case savescan::FixState::Refused: return "REFUSED";
	case savescan::FixState::Restored: return "PUT BACK";
	case savescan::FixState::Held: return "held by the game";
	}
	return "?";
}

// The FixDone of a result; into the caller's struct as far as its size goes.
void Answer(const savescan::FixOut& out, savescan::FixDone* now, savescan::DoneFn done, void* user) {
	const savescan::FixDone d { sizeof(savescan::FixDone), out.ok ? 1 : 0, out.dryRun ? 1 : 0, static_cast<uint32_t>(out.files.size()), out.changed, out.itemsChanged, out.dropped, out.renumbered, out.resized, out.unreadable,
		out.restored, out.notRestored, nullptr, nullptr, out.affixesRenumbered, out.affixesDropped };
	if (now != nullptr) {
		g_nowBackup = out.backup;
		g_nowWhy    = out.why;
		savescan::FixDone copy = d;
		copy.backup            = g_nowBackup.c_str();
		copy.why               = g_nowWhy.c_str();
		const uint32_t size    = now->size < sizeof(copy) ? now->size : static_cast<uint32_t>(sizeof(copy));
		copy.size              = size;
		memcpy(now, &copy, size);
	}
	if (done != nullptr) {
		savescan::FixDone call = d;
		call.backup            = out.backup.c_str();
		call.why               = out.why.c_str();
		done(user, &call);
	}
}

void Finish(FixJob& job, savescan::FixOut& out) {
	const bool dry = (job.request.flags & savescan::kDryRun) != 0;
	out.dryRun     = dry;
	std::string report;
	for (const savescan::FixFileOut& file : out.files) {
		char line[700];
		snprintf(line, sizeof(line), "  %s: %s, %u items, %u changed (%u dropped, %u renumbered, %u re-encoded; affixes %u renumbered, %u dropped), %d -> %d bytes%s%s", file.name.c_str(), StateName(file.state), file.items,
			file.itemsChanged, file.dropped, file.renumbered, file.resized, file.affixesRenumbered, file.affixesDropped, file.bytesBefore, file.bytesAfter, file.why.empty() ? "" : " - ", file.why.c_str());
		LogF("%s", line);
		report += std::string(line) + "\r\n";
		if (!file.note.empty()) {
			LogF("    %s", file.note.c_str());
			report += "    " + file.note + "\r\n";
		}
		if (job.request.file != nullptr) {
			const savescan::FileReport one { sizeof(savescan::FileReport), file.name.c_str(), static_cast<savescan::FileState>(file.state), file.items, file.itemsChanged, file.dropped, file.renumbered, file.resized,
				file.bytesBefore, file.bytesAfter, file.why.c_str(), file.note.c_str(), file.affixesRenumbered, file.affixesDropped };
			job.request.file(job.request.user, &one);
		}
	}
	LogF("fixSaves%s: %s - %u files, %u changed, %u items, %u dropped, %u renumbered, %u re-encoded, affixes %u/%u, %u unreadable, %u put back%s%s%s%s", dry ? " (dry run)" : "", out.ok ? "ok" : "NOT DONE",
		static_cast<unsigned>(out.files.size()), out.changed, out.itemsChanged, out.dropped, out.renumbered, out.resized, out.affixesRenumbered, out.affixesDropped, out.unreadable, out.restored, out.why.empty() ? "" : " - ",
		out.why.c_str(), out.backup.empty() ? "" : "; backup ", out.backup.c_str());
	if (!out.backup.empty()) { // report.txt marks the fix as finished (the start-up check looks for a plan without it)
		WriteOwn(Widen(out.backup) + L"\\report.txt", std::string(out.ok ? "Done: every changed file was written, read back and checked.\r\n" : "NOT DONE: ") + (out.ok ? "" : out.why + "\r\n") + report);
	}
	Answer(out, job.now, job.request.done, job.request.user);
}

// The work of a fix, on whatever thread runs it (never waits for another thread).
void Execute(FixJob& job) {
	savescan::FixOut out;
	if (!job.refusal.empty()) {
		out.why = job.refusal;
		Finish(job, out);
		return;
	}
	std::string problem;
	if (!ItemTables(job.shapes[0], problem)) {
		out.why = "Nothing was written: " + problem + ".";
		Finish(job, out);
		return;
	}
	for (int i = 1; i < job.count; ++i) {
		job.shapes[i].items = job.shapes[0].items;
	}
	std::vector<savescan::FixInput> files;
	for (const wchar_t* pattern : { L"\\*.d2s", L"\\*.d2i" }) {
		WIN32_FIND_DATAW entry {};
		const HANDLE     find = FindFirstFileW((job.saves + pattern).c_str(), &entry);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
				continue;
			}
			savescan::FixInput file;
			file.name  = Narrow(entry.cFileName);
			file.stash = pattern[3] == L'd' && pattern[5] == L'i';
			const uint64_t ticks = static_cast<uint64_t>(entry.ftLastWriteTime.dwHighDateTime) << 32 | entry.ftLastWriteTime.dwLowDateTime;
			file.writeTime       = ticks > 116444736000000000ull ? static_cast<int64_t>((ticks - 116444736000000000ull) / 10000000ull) : 0;
			if (ReadWhole(job.saves + L"\\" + entry.cFileName, file.bytes)) {
				file.tables = &job.shapes[0];
				for (int i = 0; i < job.count; ++i) { // the data version whose table reads it
					const auto* data = reinterpret_cast<const uint8_t*>(file.bytes.data());
					if ((file.stash ? savescan::ReadStash(data, file.bytes.size(), job.shapes[i]) : savescan::ReadCharacter(data, file.bytes.size(), job.shapes[i])).ok) {
						file.tables = &job.shapes[i];
						break;
					}
				}
			}
			files.push_back(std::move(file));
		} while (FindNextFileW(find, &entry) != 0);
		FindClose(find);
	}
	WinIo io;
	io.saves = job.saves;
	io.own   = job.own;
	out      = savescan::Fix(files, job.changes, job.resizes, (job.request.flags & savescan::kDryRun) != 0, io, job.affixes, job.notAfter);
	Finish(job, out);
}

DWORD WINAPI FixThread(void* parameter) {
	FixJob* job = static_cast<FixJob*>(parameter);
	Execute(*job);
	delete job;
	InterlockedExchange(&g_fixBusy, 0);
	return 0;
}

// The request, copied and checked on the caller's thread (with the game's stat table). nullptr: bad arguments.
FixJob* Prepare(const savescan::FixRequest* request) {
	if (g_ctx == nullptr || request == nullptr || request->size < offsetof(savescan::FixRequest, affixChanges)) {
		return nullptr;
	}
	auto* job = new FixJob;
	memcpy(&job->request, request, request->size < sizeof(savescan::FixRequest) ? request->size : sizeof(savescan::FixRequest));
	if (request->size < offsetof(savescan::FixRequest, before)) {
		job->request.affixChanges     = nullptr;
		job->request.affixChangeCount = 0;
	}
	if (request->size < sizeof(savescan::FixRequest)) {
		job->request.before      = nullptr;
		job->request.beforeCount = 0;
		job->request.notAfter    = 0;
	}
	const savescan::FixRequest& r = job->request;
	if ((r.changeCount != 0 && r.changes == nullptr) || (r.affixChangeCount != 0 && r.affixChanges == nullptr) || (r.beforeCount != 0 && r.before == nullptr) || (r.resizeCount != 0 && r.resizes == nullptr)) {
		job->refusal = "A change list is missing (its count is not 0): nothing was written.";
	}
	for (uint32_t i = 0; job->refusal.empty() && i < r.changeCount; ++i) {
		job->changes.push_back({ r.changes[i].from, r.changes[i].to });
	}
	for (uint32_t i = 0; job->refusal.empty() && i < r.resizeCount; ++i) {
		job->resizes.push_back({ r.resizes[i].stat, r.resizes[i].saveBits, r.resizes[i].paramBits, r.resizes[i].saveAdd });
	}
	for (uint32_t i = 0; job->refusal.empty() && i < r.affixChangeCount; ++i) {
		job->affixes.push_back({ r.affixChanges[i].suffix, r.affixChanges[i].from, r.affixChanges[i].to });
	}
	for (uint32_t i = 0; job->refusal.empty() && i < r.beforeCount; ++i) {
		job->before.push_back({ r.before[i].stat, r.before[i].saveBits, r.before[i].paramBits, r.before[i].saveAdd });
	}
	job->notAfter = r.notAfter;
	const bool dry = (r.flags & savescan::kDryRun) != 0;
	if (job->refusal.empty() && !dry) {
		const int inGame = InGame();
		if (inGame != 0) {
			job->refusal = inGame > 0 ? "A character is in a game: leave to the main menu to fix the saves (the game holds that character and its stash). Nothing was written."
			                          : "Save Scan cannot tell whether a character is in a game (D2RCore's IsInGame was not found), so it writes nothing.";
		}
	}
	if (job->refusal.empty()) {
		uintptr_t   base = 0;
		Scan        unused;
		std::string problem;
		if (!GameKnown(base, problem) || !GameTables(base, job->shapes, job->versions, job->count, unused, problem, false)) {
			job->refusal = "The game's stat table could not be read (" + problem + "). Nothing was written.";
		}
		for (int i = 0; job->refusal.empty() && i < job->count; ++i) {
			if (!savescan::ApplyBefore(job->shapes[i], job->before, problem)) {
				job->refusal = "Nothing was written: " + problem + ".";
			}
		}
	}
	job->saves = SaveFolder();
	job->own   = OwnFolder();
	if (job->refusal.empty() && !IsDirectory(job->saves)) {
		job->refusal = "The save folder was not found (" + Narrow(job->saves) + "). Nothing was written.";
	}
	LogF("fix%s: %u stat changes, %u new shapes, %u affix changes, %u before shapes%s", dry ? " (dry run)" : "", r.changeCount, r.resizeCount, r.affixChangeCount, r.beforeCount, job->refusal.empty() ? "" : (" - REFUSED: " + job->refusal).c_str());
	return job;
}

constexpr const char* kBusy = "Save Scan is busy with another fix of the saves: try again when it is done. Nothing was written.";

int __cdecl ApiFixSaves(const savescan::FixRequest* request) noexcept {
	if (request == nullptr || request->done == nullptr) {
		return 0;
	}
	FixJob* job = Prepare(request);
	if (job == nullptr) {
		return 0;
	}
	if (InterlockedCompareExchange(&g_fixBusy, 1, 0) != 0) { // say so from a thread of its own (done() never comes on the caller's thread)
		job->refusal        = kBusy;
		const HANDLE thread = CreateThread(nullptr, 0, [](void* p) -> DWORD {
			FixJob* j = static_cast<FixJob*>(p);
			Execute(*j);
			delete j;
			return 0;
		}, job, 0, nullptr);
		if (thread == nullptr) {
			delete job;
			return 0;
		}
		CloseHandle(thread);
		return 1;
	}
	if (g_fixThread != nullptr) {
		CloseHandle(g_fixThread);
	}
	g_fixThread = CreateThread(nullptr, 0, FixThread, job, 0, nullptr);
	if (g_fixThread == nullptr) {
		delete job;
		InterlockedExchange(&g_fixBusy, 0);
		return 0;
	}
	return 1;
}

int __cdecl ApiFixSavesNow(const savescan::FixRequest* request, savescan::FixDone* out) noexcept {
	if (out == nullptr || out->size < offsetof(savescan::FixDone, affixesRenumbered)) {
		return 0;
	}
	FixJob* job = Prepare(request);
	if (job == nullptr) {
		return 0;
	}
	job->now        = out;
	const bool mine = InterlockedCompareExchange(&g_fixBusy, 1, 0) == 0;
	if (!mine) {
		job->refusal = kBusy;
	}
	Execute(*job);
	delete job;
	if (mine) {
		InterlockedExchange(&g_fixBusy, 0);
	}
	return 1;
}

std::vector<std::string> Lines(const std::string& text) {
	std::vector<std::string> lines;
	size_t                   from = 0;
	while (from < text.size()) {
		size_t end = text.find('\n', from);
		end        = end == std::string::npos ? text.size() : end;
		std::string line = text.substr(from, end - from);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		lines.push_back(line);
		from = end + 1;
	}
	return lines;
}

// Puts back what the fix of this backup wrote. On the caller's thread.
savescan::FixOut RestoreFolder(const std::wstring& folder) {
	savescan::FixOut out;
	const std::wstring own = OwnFolder() + L"\\backup-";
	if (folder.size() <= own.size() || _wcsnicmp(folder.c_str(), own.c_str(), own.size()) != 0 || folder.find(L"..") != std::wstring::npos || !IsDirectory(folder)) {
		out.why = "Nothing was put back: " + Narrow(folder) + " is not one of Save Scan's backups (" + Narrow(own) + "...).";
		return out;
	}
	out.backup = Narrow(folder);
	std::string plan, where;
	if (!ReadWhole(folder + L"\\written.txt", plan)) {
		out.why = "Nothing was put back: the backup has no written.txt (a fix from before Save Scan 0.3.4, or one that wrote nothing); copy its files back by hand if needed.";
		return out;
	}
	ReadWhole(folder + L"\\where-each-file-came-from.txt", where);
	const std::string saves = Narrow(SaveFolder());
	for (const std::string& line : Lines(where)) {
		const size_t tab = line.find('\t');
		if (tab == std::string::npos) {
			continue;
		}
		const std::string path  = line.substr(tab + 1);
		const size_t      slash = path.find_last_of('\\');
		if (slash == std::string::npos || _stricmp(path.substr(0, slash).c_str(), saves.c_str()) != 0) {
			out.why = "Nothing was put back: the backup is of another save folder (" + path.substr(0, slash == std::string::npos ? 0 : slash) + "), not " + saves + ".";
			return out;
		}
	}
	std::vector<savescan::RestoreEntry> entries;
	for (const std::string& line : Lines(plan)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
		if (b == std::string::npos) {
			continue;
		}
		savescan::RestoreEntry entry;
		entry.name        = line.substr(0, a);
		entry.writtenSize = strtoull(line.c_str() + a + 1, nullptr, 10);
		entry.writtenHash = strtoull(line.c_str() + b + 1, nullptr, 16);
		if (entry.name.find_first_of("\\/") != std::string::npos || !ReadWhole(folder + L"\\" + Widen(entry.name), entry.backup)) {
			out.why = "Nothing was put back: the backup's copy of " + entry.name + " cannot be read.";
			return out;
		}
		entries.push_back(std::move(entry));
	}
	WinIo io;
	io.saves = SaveFolder();
	io.own   = OwnFolder();
	out        = savescan::Restore(entries, io);
	out.backup = Narrow(folder);
	std::string report = std::string(out.ok ? "Restored: " : "NOT ALL RESTORED: ") + out.why + "\r\n";
	for (const savescan::FixFileOut& file : out.files) {
		report += "  " + file.name + ": " + StateName(file.state) + (file.why.empty() ? "" : " - " + file.why) + "\r\n";
		LogF("  restore %s: %s%s%s", file.name.c_str(), StateName(file.state), file.why.empty() ? "" : " - ", file.why.c_str());
	}
	WriteOwn(folder + L"\\restored.txt", report);
	LogF("restore %s: %s", Narrow(folder).c_str(), out.why.c_str());
	return out;
}

int __cdecl ApiRestore(const char* backupFolder, savescan::FixDone* out) noexcept {
	if (g_ctx == nullptr || backupFolder == nullptr || out == nullptr || out->size < offsetof(savescan::FixDone, affixesRenumbered)) {
		return 0;
	}
	savescan::FixOut result;
	if (InterlockedCompareExchange(&g_fixBusy, 1, 0) != 0) {
		result.why = "Save Scan is busy with a fix of the saves: try again when it is done. Nothing was put back.";
	} else {
		result = RestoreFolder(Widen(backupFolder));
		InterlockedExchange(&g_fixBusy, 0);
	}
	Answer(result, out, nullptr, nullptr);
	return 1;
}

constexpr savescan::Api kApi { sizeof(savescan::Api), savescan::kApiVersion, ApiScan, ApiDump, ApiFixSaves, ApiScanAffixes, ApiFixSavesNow, ApiRestore };


// ---------------------------------------------------------------- at the main menu: alarms

struct Alarm {
	std::string text;
	std::string restore; // a backup folder to offer putting back ("" = none)
};
SRWLOCK            g_alarmLock = SRWLOCK_INIT;
std::vector<Alarm> g_alarms;
std::string        g_lastAction; // what the last button did (UI thread)
const modbook::Api* g_book = nullptr;
int                 g_tab  = 0;

void SetAlarms(std::vector<Alarm> alarms) {
	AcquireSRWLockExclusive(&g_alarmLock);
	g_alarms = std::move(alarms);
	ReleaseSRWLockExclusive(&g_alarmLock);
	if (g_book != nullptr && g_tab != 0) {
		g_book->redraw(g_tab);
	}
}

// A fix cut off (its plan written.txt, but no report.txt and no restored.txt), and saves that
// hold made stats the game's table lacks now (Live Tables not installed, or its rows not laid in).
std::vector<Alarm> FindAlarms() {
	std::vector<Alarm> alarms;
	const std::wstring own = OwnFolder();
	WIN32_FIND_DATAW   entry {};
	const HANDLE       find = InterlockedCompareExchange(&g_fixBusy, 0, 0) == 0 ? FindFirstFileW((own + L"\\backup-*").c_str(), &entry) : INVALID_HANDLE_VALUE;
	if (find != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring folder = own + L"\\" + entry.cFileName;
			if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || GetFileAttributesW((folder + L"\\written.txt").c_str()) == INVALID_FILE_ATTRIBUTES
			    || GetFileAttributesW((folder + L"\\report.txt").c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW((folder + L"\\restored.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
				continue;
			}
			alarms.push_back({ "A fix of the saves was CUT OFF (" + Narrow(entry.cFileName) + "): some saves may have the new stat numbers while the table has the old ones. Put the backup back before loading a character.", Narrow(folder) });
		} while (FindNextFileW(find, &entry) != 0);
		FindClose(find);
	}
	std::string grown;
	if (ReadWhole(LoaderFolder() + L"\\live-suite\\live-tables\\" + Widen(ModName().empty() ? "no mod" : ModName()) + L"\\grown-rows.txt", grown)) {
		const std::vector<savescan::MadeRow> rows = savescan::MadeStatRows(grown);
		uintptr_t                            base = 0;
		savescan::Tables                     shapes[3];
		int                                  versions[3] = {}, count = 0;
		Scan                                 unused;
		std::string                          problem;
		if (!rows.empty() && GameKnown(base, problem) && GameTables(base, shapes, versions, count, unused, problem, false) && ItemTables(shapes[0], problem)) {
			std::vector<savescan::FixInput> files;
			const std::wstring              saves = SaveFolder();
			for (const wchar_t* pattern : { L"\\*.d2s", L"\\*.d2i" }) {
				WIN32_FIND_DATAW file {};
				const HANDLE     list = FindFirstFileW((saves + pattern).c_str(), &file);
				if (list == INVALID_HANDLE_VALUE) {
					continue;
				}
				do {
					savescan::FixInput in;
					in.name   = Narrow(file.cFileName);
					in.stash  = pattern[3] == L'd' && pattern[5] == L'i';
					in.tables = &shapes[0];
					if (ReadWhole(saves + L"\\" + file.cFileName, in.bytes)) {
						files.push_back(std::move(in));
					}
				} while (FindNextFileW(list, &file) != 0);
				FindClose(list);
			}
			std::vector<savescan::MadeRow> missing;
			const std::vector<std::string> holding = savescan::FilesHoldingMissing(files, shapes[0], rows, missing);
			if (!missing.empty()) {
				std::string stats, names;
				for (const savescan::MadeRow& row : missing) {
					stats += (stats.empty() ? "" : ", ") + (row.name.empty() ? std::string("stat") : row.name) + " (" + std::to_string(row.number) + ")";
				}
				for (const std::string& name : holding) {
					names += (names.empty() ? "" : ", ") + name;
				}
				const bool loaded = GetModuleHandleW(L"d2rl-live-tables.dll") != nullptr;
				alarms.push_back({ std::string(loaded ? "Live Tables is loaded but its made stats are NOT in the game's table (its hook is off - see its log): "
				                                      : "Live Tables is NOT installed, so its made stats are not in the game's table: ")
				                       + stats + "." + (holding.empty() ? " No save holds one." : " Do NOT load " + names + ": they hold those stats, and the game would read them wrong or refuse them."),
					"" });
			}
		} else if (!rows.empty()) {
			alarms.push_back({ "Save Scan could not check the saves for Live Tables' made stats: " + problem, "" });
		}
	}
	return alarms;
}

void CheckAtMenu() {
	std::vector<Alarm> alarms = FindAlarms();
	for (const Alarm& alarm : alarms) {
		LogF("ALARM: %s", alarm.text.c_str());
		if (g_ctx != nullptr) {
			g_ctx->WriteConsoleMessage(("Save Scan: " + alarm.text + (alarm.restore.empty() ? "" : " (console: savescan restore " + alarm.restore + ")")).c_str());
		}
	}
	const bool any = !alarms.empty();
	SetAlarms(std::move(alarms));
	if (any && g_book != nullptr && g_tab != 0) {
		g_book->open(g_tab);
	}
}

void __cdecl TabDraw(void*, modbook::Page* page) noexcept {
	AcquireSRWLockShared(&g_alarmLock);
	const std::vector<Alarm> alarms = g_alarms;
	ReleaseSRWLockShared(&g_alarmLock);
	int y = modbook::kActionY;
	if (modbook::HasRows(g_book)) {
		y = g_book->action(page, "Check", "Check again", nullptr, 1);
	} else {
		g_book->button(page, "Check", 82, y, 232, "Check again", "$FontColorGoldYellow");
		y += modbook::kActionRowStep;
	}
	if (alarms.empty()) {
		g_book->label(page, "None", 82, y, 990, 200, "$FontColorLightGold", "left", 1,
			"Nothing to report: no cut-off fix of the saves, and no save holds a made stat the game's table lacks.\nSave Scan reads the saves only when asked; it rewrites them only when Live Tables or Live Properties changes a stat row (after a dated backup).");
	}
	for (size_t i = 0; i < alarms.size() && i < 6; ++i) {
		const std::string name = "Alarm" + std::to_string(i);
		g_book->label(page, name.c_str(), 82, y, 990, 150, "$FontColorLightRed", "left", 1, alarms[i].text.c_str());
		y += 160;
		if (!alarms[i].restore.empty()) {
			g_book->button(page, ("Restore" + std::to_string(i)).c_str(), 82, y, 470, "Put the backup back", "$FontColorGoldYellow");
			y += modbook::kActionRowStep + 10;
		}
	}
	if (!g_lastAction.empty()) {
		g_book->label(page, "Last", 82, y + 10, 990, 200, "$FontColorWhite", "left", 1, g_lastAction.c_str());
	}
}
void __cdecl TabClick(void*, const char* what) noexcept {
	const std::string name = what != nullptr ? what : "";
	if (name == "Check") {
		CheckAtMenu();
		g_lastAction = "Checked again.";
	} else if (name.rfind("Restore", 0) == 0) {
		const size_t index = strtoul(name.c_str() + 7, nullptr, 10);
		std::string  folder;
		AcquireSRWLockShared(&g_alarmLock);
		folder = index < g_alarms.size() ? g_alarms[index].restore : "";
		ReleaseSRWLockShared(&g_alarmLock);
		if (!folder.empty()) {
			savescan::FixDone done { sizeof(done) };
			ApiRestore(folder.c_str(), &done);
			g_lastAction = std::string(done.ok != 0 ? "Put back: " : "NOT ALL PUT BACK: ") + (done.why != nullptr ? done.why : "");
			CheckAtMenu();
		}
	} else if (name == "Escape" && g_book != nullptr) {
		g_book->close(modbook::SideOf(g_book, g_tab, modbook::kLeft));
	}
	if (g_book != nullptr) {
		g_book->redraw(g_tab);
	}
}
void __cdecl TabKeys(void*, const char*) noexcept {}
void __cdecl TabMove(void*, int, int, int) noexcept {}
void __cdecl TabShown(void*, int) noexcept {}
int __cdecl  TabLeave(void*) noexcept {
	return 1;
}

void BookSetUp() {
	if (g_book != nullptr) {
		return;
	}
	g_book = modbook::Find();
	if (g_book == nullptr) {
		return;
	}
	const modbook::Tab tab { sizeof(modbook::Tab), modbook::kLeft, "saves", "Saves", nullptr, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr };
	g_tab = g_book->addTab(&tab);
	LogF("Mod Book: tab Saves (left, %s)", g_tab != 0 ? "added" : "REFUSED, see mod-book.log");
}

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	BookSetUp();
	if (InGame() == 0) { // the tables are read at the start, before a character loads
		CheckAtMenu();
	}
}

// This DLL's own file: sha256 (bcrypt), for the start line.
std::string OwnSha256() {
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnSha256), &self)) {
		return "?";
	}
	wchar_t     path[1024] = {};
	const DWORD length     = GetModuleFileNameW(self, path, 1000);
	std::string bytes;
	if (length == 0 || !ReadWhole(std::wstring(path, length), bytes)) {
		return "?";
	}
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	uint8_t           hash[32]  = {};
	bool              ok        = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
	          && BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), hash, sizeof(hash)) == 0;
	if (algorithm != nullptr) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	if (!ok) {
		return "?";
	}
	char text[65];
	for (int i = 0; i < 32; ++i) {
		snprintf(text + i * 2, 3, "%02x", hash[i]);
	}
	return text;
}

// ---------------------------------------------------------------- console

void __cdecl SayHit(void*, const savescan::Hit* hit) noexcept {
	char text[300];
	snprintf(text, sizeof(text), "Save Scan:   %s: %s (%s) has stat %u = %d%s", hit->file, hit->code, hit->where, hit->stat, hit->value, hit->param != 0 ? (" param " + std::to_string(hit->param)).c_str() : "");
	g_ctx->WriteConsoleMessage(text);
}

void __cdecl SayProblem(void*, const char* file, const char* why) noexcept {
	g_ctx->WriteConsoleMessage((std::string("Save Scan:   ") + (file[0] != '\0' ? std::string("UNREADABLE ") + file + ": " : std::string("could not run: ")) + why).c_str());
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	savescan::Summary summary { sizeof(summary) };
	char              text[1200];
	if (args.find("dump") != std::string::npos) {
		char path[1024] = "";
		if (ApiDump(path, sizeof(path), &summary, SayProblem, nullptr) != 0) {
			snprintf(text, sizeof(text), "Save Scan: %u files (%u unreadable), %u items written to %s", summary.files, summary.unreadable, summary.items, path);
			g_ctx->WriteConsoleMessage(text);
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.rfind("restore", 0) == 0) {
		std::string folder = args.size() > 8 ? args.substr(8) : "";
		while (!folder.empty() && (folder.back() == ' ' || folder.back() == '\t')) {
			folder.pop_back();
		}
		if (!folder.empty() && folder.find('\\') == std::string::npos) {
			folder = Narrow(OwnFolder()) + "\\" + folder; // a backup's folder name alone
		}
		savescan::FixDone done { sizeof(done) };
		if (folder.empty()) {
			g_ctx->WriteConsoleMessage("Save Scan: savescan restore <backup folder> (as an alarm or a fix's report names it)");
		} else if (ApiRestore(folder.c_str(), &done) != 0) {
			g_ctx->WriteConsoleMessage((std::string("Save Scan: ") + (done.ok != 0 ? "" : "NOT ALL PUT BACK: ") + (done.why != nullptr ? done.why : "")).c_str());
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.rfind("check", 0) == 0) {
		CheckAtMenu();
		AcquireSRWLockShared(&g_alarmLock);
		const size_t count = g_alarms.size();
		ReleaseSRWLockShared(&g_alarmLock);
		if (count == 0) {
			g_ctx->WriteConsoleMessage("Save Scan: nothing to report (no cut-off fix, no save holding a made stat the table lacks)");
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.find("remove") != std::string::npos) {
		const unsigned long stat = strtoul(args.c_str() + args.find("remove") + 6, nullptr, 10);
		static std::vector<savescan::StatChange> changes;
		changes.clear();
		changes.push_back({ static_cast<uint16_t>(stat), savescan::kDropped });
		for (unsigned long s = stat + 1; s <= 510; ++s) {
			changes.push_back({ static_cast<uint16_t>(s), static_cast<int32_t>(s - 1) });
		}
		uintptr_t        base = 0;
		savescan::Tables shapes[3];
		int              versions[3] = {}, count = 0;
		Scan             unused;
		std::string      problem;
		if (!GameKnown(base, problem) || !GameTables(base, shapes, versions, count, unused, problem, false)) {
			g_ctx->WriteConsoleMessage(("Save Scan: " + problem).c_str());
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (stat >= shapes[0].stats.size() || shapes[0].stats.size() > 511) {
			g_ctx->WriteConsoleMessage("Save Scan: savescan remove <stat number of the table>");
			return D2RL::ConsoleCommandResult::Handled;
		}
		changes.resize(shapes[0].stats.size() - stat); // only the stats the table has
		savescan::FixRequest request { sizeof(request) };
		request.flags       = savescan::kDryRun;
		request.changes     = changes.data();
		request.changeCount = static_cast<uint32_t>(changes.size());
		request.done        = [](void*, const savescan::FixDone*) noexcept {};
		if (ApiFixSaves(&request) == 0) {
			g_ctx->WriteConsoleMessage("Save Scan: savescan remove <stat number of the table>");
		} else {
			g_ctx->WriteConsoleMessage("Save Scan: dry run started (nothing is written); what would change is in the plugin log");
		}
		return D2RL::ConsoleCommandResult::Handled;
	}
	std::vector<uint16_t> stats;
	for (const char* at = args.c_str(); *at != '\0';) {
		char*               end   = nullptr;
		const unsigned long value = strtoul(at, &end, 10);
		if (end == at) {
			if (*at != ' ' && *at != '\t') {
				g_ctx->WriteConsoleMessage("Save Scan: savescan | savescan <stat number> [more numbers] | savescan dump | savescan remove <stat number> (dry run) | savescan check | savescan restore <backup folder>");
				return D2RL::ConsoleCommandResult::Handled;
			}
			++at;
			continue;
		}
		stats.push_back(static_cast<uint16_t>(value));
		at = end;
	}
	if (ApiScan(stats.data(), static_cast<uint32_t>(stats.size()), &summary, SayHit, SayProblem, nullptr) != 0) {
		if (stats.empty()) {
			snprintf(text, sizeof(text), "Save Scan: %u files, %u items read, %u files unreadable. savescan <stat number> finds a stat's items; savescan dump writes everything to a file.", summary.files, summary.items,
				summary.unreadable);
		} else {
			snprintf(text, sizeof(text), "Save Scan: %u item%s hold%s it, in %u files (%u items)%s", summary.holders, summary.holders == 1 ? "" : "s", summary.holders == 1 ? "s" : "", summary.files, summary.items,
				summary.unreadable != 0 ? ". Some files were UNREADABLE: what they hold is not known." : ".");
		}
		g_ctx->WriteConsoleMessage(text);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "save-scan",
	.name        = "Save Scan",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Reads every saved item (all characters, the shared stash) when another plugin or the console asks which items hold a stat, and rewrites them (after a backup) when a stat is removed, renumbered or resized. Does nothing otherwise.",
	.flags       = D2RL::PluginFlags::Shared,
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
	static_cast<void>(context->RegisterConsoleCommand("savescan", OnConsole, "Save Scan: which saved items hold a stat (savescan <stat number>), or everything to a file (savescan dump)"));
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, registerDataTablesLoadedListener))) {
		D2RL::Lifecycle::DataTablesLoadedListener tables {};
		tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
		tables.callback   = OnTablesLoaded;
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
	}
	BookSetUp();
	LogF("Save Scan %s (Api %u) sha256 %s: ready - it reads the saves only when asked and checks them at the main menu (console: savescan; other plugins: save_scan_api.h; Mod Book tab Saves)", kVersion, savescan::kApiVersion,
		OwnSha256().c_str());
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	if (g_fixThread != nullptr) { // a fix still writing: let it end (the files are written whole, one by one)
		WaitForSingleObject(g_fixThread, 20000);
		CloseHandle(g_fixThread);
		g_fixThread = nullptr;
	}
	g_ctx = nullptr;
}

// The Api, for other plugins (save_scan_api.h: savescan::Find).
D2RL_PLUGIN_EXPORT const savescan::Api* __cdecl SaveScanGetApi(uint32_t version) noexcept {
	// The newest table to every caller (version >= 1): it checks version and size (save_scan_api.h Find).
	return version >= 1 && g_ctx != nullptr ? &kApi : nullptr;
}
