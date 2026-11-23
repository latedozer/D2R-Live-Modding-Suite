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
// Saves are opened for reading only. The stat table is the one the game holds in memory at
// the moment of the call (so stats changed or made at run time are read as the game saves
// them); the item tables (armor, weapons, misc) are read from the mod's text files.
// The reader itself is save_core.cpp.

#include <D2RLPlugin/api.h>

#include "save_core.h"
#include "save_scan_api.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.1.0";

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

// <loader folder>\save-scan\<mod>: beside the plugins folder. Never inside a mod's .mpq.
std::wstring OwnFolder() {
	return LoaderFolder() + L"\\save-scan\\" + Widen(ModName().empty() ? "no mod" : ModName());
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
bool GameTables(uintptr_t base, savescan::Tables shapes[3], int versions[3], int& count, Scan& scan, std::string& problem) {
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
		if (count == 0) { // names and properties for the dump, from the newest
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

constexpr savescan::Api kApi { sizeof(savescan::Api), savescan::kApiVersion, ApiScan, ApiDump };

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
	std::vector<uint16_t> stats;
	for (const char* at = args.c_str(); *at != '\0';) {
		char*               end   = nullptr;
		const unsigned long value = strtoul(at, &end, 10);
		if (end == at) {
			if (*at != ' ' && *at != '\t') {
				g_ctx->WriteConsoleMessage("Save Scan: savescan | savescan <stat number> [more numbers] | savescan dump");
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
	.description = "Reads every saved item (all characters, the shared stash) when another plugin or the console asks which items hold a stat. Does nothing otherwise.",
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
	LogF("Save Scan %s: ready - it works only when asked (console: savescan; other plugins: save_scan_api.h)", kVersion);
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_ctx = nullptr;
}

// The Api, for other plugins (save_scan_api.h: savescan::Find).
D2RL_PLUGIN_EXPORT const savescan::Api* __cdecl SaveScanGetApi(uint32_t version) noexcept {
	return version == savescan::kApiVersion && g_ctx != nullptr ? &kApi : nullptr;
}
