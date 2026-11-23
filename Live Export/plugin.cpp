// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Export: writes the mod being played, with the Live Suite's edits in it, as files.
//
//   * a whole mod folder:  d2rloader\live-suite\mod-export\<name>\<name>.mpq\   (copy the <name>
//     folder into the game's mods folder and start the game with -mod <name>). Every file of the
//     mod; the tables Exalted Items keeps adjusted copies of are taken from those copies (what the
//     game really reads), unless "plain" is asked for; then the Live Suite's edits are written
//     into the tables. Tables the mod does not have are taken from the game's own data (CASC).
//   * a D2RMM mod:         d2rloader\live-suite\mod-export\<name>-d2rmm\  (mod.json + mod.ts):
//     the same edits, made to whatever tables D2RMM hands it, row by row, so other mods' changes
//     stay. Copy the folder into D2RMM's mods folder.
//
// What the edits are: d2rloader\live-suite\live-properties\<mod>\ (property-edits.txt,
// stat-edits.txt, affix-edits.txt, string-keys.txt) and live-cube-recipes\<mod>\ (recipes.txt,
// recipes-classic-lod.txt). The logic is export_core.cpp. Nothing is written outside
// d2rloader\live-suite\mod-export; the mod's own folder is only read.
//
// Console (Ctrl + `): export | export mpq [name] [plain] | export d2rmm [name]
// Mod Book tab "Export".

#include <D2RLPlugin/api.h>

#include "export_core.h"

#include "../Mod Book/mod_book_api.h"

#include <windows.h>

#include <CascLib.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.2.0";

const D2RL::PluginContext*   g_ctx     = nullptr;
const D2RL::ThreadService*   g_threads = nullptr;

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

// ---------------------------------------------------------------- files

std::wstring Widen(const std::string& text) {
	const int    n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
	return out;
}
std::string Narrow(const std::wstring& text) {
	const int   n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring GameDirectory() {
	wchar_t      path[1024] = {};
	const DWORD  length     = GetModuleFileNameW(nullptr, path, 1000);
	std::wstring out(path, length < 1000 ? length : 0);
	const size_t slash = out.find_last_of(L'\\');
	return slash != std::wstring::npos ? out.substr(0, slash) : out;
}

// The loader folder: beside the plugins folder this DLL was loaded from, else the game's d2rloader.
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

bool IsDirectory(const std::wstring& path) {
	const DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool IsFile(const std::wstring& path) {
	const DWORD a = GetFileAttributesW(path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// The mod's .mpq folder (the one that holds data\ and modinfo.json).
bool FindModFolder(std::wstring& folder) {
	const std::wstring mod = Widen(ModName());
	if (mod.empty()) {
		return false;
	}
	std::vector<std::wstring> places;
	if (g_ctx->modDirectory != nullptr && g_ctx->modDirectory[0] != L'\0') {
		std::wstring base = g_ctx->modDirectory;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
			base.pop_back();
		}
		places.push_back(base + L"\\" + mod + L".mpq");
		places.push_back(base);
	}
	places.push_back(GameDirectory() + L"\\mods\\" + mod + L"\\" + mod + L".mpq");
	for (const std::wstring& place : places) {
		if (IsDirectory(place + L"\\data")) {
			folder = place;
			return true;
		}
	}
	return false;
}

bool ReadWhole(const std::wstring& path, std::string& text) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER size {};
	bool          ok = GetFileSizeEx(file, &size) && size.QuadPart < (512LL << 20);
	text.assign(ok ? static_cast<size_t>(size.QuadPart) : 0, '\0');
	DWORD got = 0;
	ok        = ok && (text.empty() || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size()));
	CloseHandle(file);
	return ok;
}

void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
	}
	CreateDirectoryW(path.c_str(), nullptr);
}

// Writes into the export folder only.
bool WriteExport(const std::wstring& path, const std::string& text) {
	if (path.find(L"\\live-suite\\mod-export\\") == std::wstring::npos) {
		return false;
	}
	MakeFolders(path.substr(0, path.find_last_of(L'\\')));
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD wrote = 0;
	const bool ok = text.empty() || (WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size());
	CloseHandle(file);
	return ok;
}

// An earlier export of the same name goes first (only ever inside mod-export).
void DeleteExport(const std::wstring& path) {
	if (path.find(L"\\live-suite\\mod-export\\") == std::wstring::npos || !IsDirectory(path)) {
		return;
	}
	WIN32_FIND_DATAW entry {};
	const HANDLE     find = FindFirstFileW((path + L"\\*").c_str(), &entry);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = entry.cFileName;
			if (name == L"." || name == L"..") {
				continue;
			}
			const std::wstring inner = path + L"\\" + name;
			if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
				DeleteExport(inner);
			} else {
				SetFileAttributesW(inner.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(inner.c_str());
			}
		} while (FindNextFileW(find, &entry) != 0);
		FindClose(find);
	}
	RemoveDirectoryW(path.c_str());
}

// Copies a folder's files into another (into the export folder only); counts what it copied.
bool CopyTree(const std::wstring& from, const std::wstring& to, size_t& files, uint64_t& bytes, std::string& problem) {
	if (to.find(L"\\live-suite\\mod-export\\") == std::wstring::npos) {
		problem = "not an export folder";
		return false;
	}
	MakeFolders(to);
	WIN32_FIND_DATAW entry {};
	const HANDLE     find = FindFirstFileW((from + L"\\*").c_str(), &entry);
	if (find == INVALID_HANDLE_VALUE) {
		return true;
	}
	bool ok = true;
	do {
		const std::wstring name = entry.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			ok = (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || CopyTree(from + L"\\" + name, to + L"\\" + name, files, bytes, problem);
		} else if (CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), FALSE)) {
			++files;
			bytes += (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
		} else {
			problem = "could not copy " + Narrow(from + L"\\" + name) + " (error " + std::to_string(GetLastError()) + ")";
			ok      = false;
		}
	} while (ok && FindNextFileW(find, &entry) != 0);
	FindClose(find);
	return ok;
}

// A file of the game's own data (CASC), by its path under data\ ("global\excel\magicprefix.txt").
bool ReadGame(HANDLE storage, const std::string& path, std::string& out) {
	HANDLE file = nullptr;
	if (storage == nullptr || !CascOpenFile(storage, ("data:data\\" + path).c_str(), 0, CASC_OPEN_BY_NAME, &file)) {
		return false;
	}
	ULONGLONG size = 0;
	bool      ok   = CascGetFileSize64(file, &size) && size < (256ULL << 20);
	if (ok) {
		out.resize(static_cast<size_t>(size));
		DWORD read = 0;
		ok         = out.empty() || (CascReadFile(file, out.data(), static_cast<DWORD>(size), &read) && read == size);
	}
	CascCloseFile(file);
	return ok;
}

// ---------------------------------------------------------------- what an export is made of

struct Sources {
	std::wstring mod;     // the mod's .mpq folder
	std::wstring exalted; // Exalted Items' adjusted copies (its data folder), "" = none
	std::wstring live;    // d2rloader\live-suite
	std::string  modName;
};

bool FindSources(Sources& s, std::string& problem) {
	s.modName = ModName();
	if (s.modName.empty() || !FindModFolder(s.mod)) {
		problem = "no mod is being played (start the game with -mod <name>), or its folder was not found";
		return false;
	}
	const std::wstring loader = LoaderFolder();
	s.live                    = loader + L"\\live-suite";
	s.exalted.clear();
	for (const std::wstring& place : { loader + L"\\exalted-items\\" + Widen(s.modName) + L"\\data", Widen("") + (g_ctx->modDirectory != nullptr ? g_ctx->modDirectory : L"") + L"\\d2rloader\\exalted-items\\" + Widen(s.modName) + L"\\data" }) {
		if (s.exalted.empty() && IsDirectory(place)) {
			s.exalted = place;
		}
	}
	return true;
}

std::string ReadEdits(const Sources& s, exporter::Edits& edits) {
	const std::wstring props = s.live + L"\\live-properties\\" + Widen(s.modName), cube = s.live + L"\\live-cube-recipes\\" + Widen(s.modName);
	std::string        text, notes;
	const auto         take = [&](const std::vector<std::string>& bad) {
        for (const std::string& b : bad) {
            notes += b + "\n";
        }
	};
	if (ReadWhole(props + L"\\property-edits.txt", text)) {
		take(exporter::ReadPropertyEdits(text, edits));
	}
	if (ReadWhole(props + L"\\stat-edits.txt", text)) {
		take(exporter::ReadStatEdits(text, edits));
	}
	if (ReadWhole(props + L"\\affix-edits.txt", text)) {
		take(exporter::ReadAffixEdits(text, edits));
	}
	if (ReadWhole(props + L"\\string-keys.txt", text)) {
		exporter::ReadStringKeys(text, edits);
	}
	if (ReadWhole(cube + L"\\recipes.txt", text)) {
		exporter::ParseTsv(text, edits.recipes);
	}
	if (ReadWhole(cube + L"\\recipes-classic-lod.txt", text)) {
		exporter::ParseTsv(text, edits.recipesBase);
	}
	return notes;
}

// A table as the game reads it: Exalted's copy (when used), the mod's file, or the game's own.
bool ReadTable(const Sources& s, bool withExalted, HANDLE storage, const std::string& path, std::string& text, std::string* from = nullptr) {
	const std::wstring w = Widen(path);
	if (withExalted && !s.exalted.empty() && ReadWhole(s.exalted + L"\\" + w, text)) {
		if (from != nullptr) {
			*from = "Exalted Items' copy";
		}
		return true;
	}
	if (ReadWhole(s.mod + L"\\data\\" + w, text)) {
		if (from != nullptr) {
			*from = "the mod";
		}
		return true;
	}
	if (from != nullptr) {
		*from = "the game";
	}
	return ReadGame(storage, path, text);
}

// The game's string number of every key the stat tables name (for stat cells that hold a number).
// Asked of D2RCore on the calling thread (the game's).
void MapStatStrings(const Sources& s, bool withExalted, exporter::Edits& edits) {
	using IdFn = uint16_t (*)(const void*);
	const HMODULE core   = GetModuleHandleW(L"D2RCore.dll");
	const auto    lookup = core != nullptr ? reinterpret_cast<IdFn>(reinterpret_cast<void*>(GetProcAddress(core, "GetNamespacedStringId"))) : nullptr;
	if (lookup == nullptr) {
		return;
	}
	struct View {
		const char* text;
		size_t      length;
	};
	const char     nonsense[] = "live-export:NoSuchKey";
	const View     none { nonsense, sizeof(nonsense) - 1 };
	const uint16_t unknown = lookup(&none);
	for (const char* table : { "global\\excel\\itemstatcost.txt", "global\\excel\\base\\itemstatcost.txt" }) {
		std::string   text;
		exporter::Tsv tsv;
		if (!ReadTable(s, withExalted, nullptr, table, text) || !exporter::ParseTsv(text, tsv)) {
			continue;
		}
		for (const char* column : { "descstrpos", "descstrneg", "descstr2", "dgrpstrpos", "dgrpstrneg", "dgrpstr2" }) {
			const int c = tsv.Column(column);
			for (size_t r = 0; c >= 0 && r < tsv.rows.size(); ++r) {
				const std::string& key = tsv.rows[r][static_cast<size_t>(c)];
				if (key.empty()) {
					continue;
				}
				const View     view { key.data(), key.size() };
				const uint16_t id = lookup(&view);
				if (id != unknown && edits.stringKeys.count(id) == 0) {
					edits.stringKeys[id] = key;
				}
			}
		}
	}
}

// Every string key the mod's (and the game's) affix name files know.
std::set<std::string> StringKeys(const std::wstring& folder) {
	std::set<std::string> keys;
	WIN32_FIND_DATAW      entry {};
	const HANDLE          find = FindFirstFileW((folder + L"\\*.json").c_str(), &entry);
	if (find == INVALID_HANDLE_VALUE) {
		return keys;
	}
	do {
		std::string text;
		if (!ReadWhole(folder + L"\\" + entry.cFileName, text)) {
			continue;
		}
		for (size_t at = text.find("\"Key\""); at != std::string::npos; at = text.find("\"Key\"", at + 5)) {
			const size_t open = text.find('"', text.find(':', at) + 1), close = open == std::string::npos ? open : text.find('"', open + 1);
			if (close != std::string::npos) {
				keys.insert(text.substr(open + 1, close - open - 1));
			}
		}
	} while (FindNextFileW(find, &entry) != 0);
	FindClose(find);
	return keys;
}

// ---------------------------------------------------------------- the exports (a thread of their own)

struct Job {
	bool            d2rmm = false, plain = false;
	std::string     name;
	Sources         sources;
	exporter::Edits edits;
	std::string     notes;
};
std::atomic<bool> g_busy { false };
SRWLOCK           g_lock = SRWLOCK_INIT;
std::vector<std::string> g_say;    // lines for the console, under g_lock
std::vector<std::string> g_status; // the tab's last result, under g_lock
const modbook::Api* g_book = nullptr;
int                 g_tab  = 0;

void Tell(const std::vector<std::string>& lines);

std::string ReportText(const exporter::Report& report) {
	std::string text;
	for (const std::string& d : report.done) {
		text += "  in:      " + d + "\r\n";
	}
	for (const std::string& p : report.problems) {
		text += "  NOT in:  " + p + "\r\n";
	}
	return text;
}

void ExportMpq(Job& job) {
	const std::wstring out = job.sources.live + L"\\mod-export\\" + Widen(job.name), mpq = out + L"\\" + Widen(job.name) + L".mpq";
	const bool         withExalted = !job.plain && !job.sources.exalted.empty();
	DeleteExport(out);
	size_t      files = 0;
	uint64_t    bytes = 0;
	std::string problem;
	if (!CopyTree(job.sources.mod, mpq, files, bytes, problem)) {
		Tell({ "Live Export: the mod could not be copied: " + problem });
		return;
	}
	size_t exaltedFiles = 0;
	if (withExalted && !CopyTree(job.sources.exalted, mpq + L"\\data", exaltedFiles, bytes, problem)) {
		Tell({ "Live Export: Exalted Items' copies could not be copied: " + problem });
		return;
	}
	HANDLE storage = nullptr;
	if (!CascOpenStorage(GameDirectory().c_str(), 0, &storage)) {
		storage = nullptr;
	}
	exporter::Report report;
	if (!withExalted) { // a plain mod: Exalted Items makes its own rows again at every start
		exporter::LeaveOutExaltedRows(job.edits, report);
	}
	// the tables (the main ones; the older data sets' base\ copies when the mod has them)
	const auto table = [&](const std::string& path, bool always, const auto& change) {
		std::string   text;
		exporter::Tsv tsv;
		const bool    present = IsFile(mpq + L"\\data\\" + Widen(path));
		if (!present && !always) {
			return;
		}
		if ((present ? !ReadWhole(mpq + L"\\data\\" + Widen(path), text) : !ReadGame(storage, path, text)) || !exporter::ParseTsv(text, tsv)) {
			report.problems.push_back(path + ": could not be read; its edits are not in the export");
			return;
		}
		if (change(tsv)) {
			WriteExport(mpq + L"\\data\\" + Widen(path), exporter::FormatTsv(tsv));
		}
	};
	const exporter::Edits& e = job.edits;
	std::vector<exporter::NewString> names;
	const std::wstring stringsFolder = mpq + L"\\data\\local\\lng\\strings";
	std::set<std::string> keys = StringKeys(stringsFolder);
	std::string gameAffixNames;
	if (!IsFile(stringsFolder + L"\\item-nameaffixes.json") && ReadGame(storage, "local\\lng\\strings\\item-nameaffixes.json", gameAffixNames)) {
		for (size_t at = gameAffixNames.find("\"Key\""); at != std::string::npos; at = gameAffixNames.find("\"Key\"", at + 5)) {
			const size_t open = gameAffixNames.find('"', gameAffixNames.find(':', at) + 1), close = gameAffixNames.find('"', open + 1);
			keys.insert(gameAffixNames.substr(open + 1, close - open - 1));
		}
	}
	const auto isKey = [&](const std::string& word) { return keys.count(word) != 0; };
	for (const char* folder : { "global\\excel\\", "global\\excel\\base\\" }) {
		const bool main = strcmp(folder, "global\\excel\\") == 0;
		if (!e.props.empty()) {
			table(std::string(folder) + "properties.txt", main, [&](exporter::Tsv& t) { exporter::ApplyProperties(t, e, report); return true; });
		}
		if (!e.stats.empty()) {
			table(std::string(folder) + "itemstatcost.txt", main, [&](exporter::Tsv& t) { exporter::ApplyStats(t, e, true, report); return true; });
		}
		static const char* const affixFiles[] = { "magicsuffix.txt", "magicprefix.txt", "automagic.txt" };
		for (int kind = 0; kind < 3; ++kind) {
			bool any = false;
			for (const exporter::AffixEdit& a : e.affixes) {
				any = any || a.kind == kind;
			}
			if (any) {
				table(std::string(folder) + affixFiles[kind], main, [&](exporter::Tsv& t) {
					std::vector<exporter::NewString> unused; // the older data sets share the main names
					exporter::ApplyAffixes(t, static_cast<exporter::AffixKind>(kind), e, isKey, main ? names : unused, report);
					return true;
				});
			}
		}
		const exporter::Tsv& list = main ? e.recipes : e.recipesBase;
		if (!list.rows.empty()) {
			table(std::string(folder) + "cubemain.txt", main, [&](exporter::Tsv& t) {
				if (withExalted) {
					return exporter::ApplyCube(t, list, report, main ? "cubemain" : "base\\cubemain");
				}
				// plain: the list without the Exalted Items rows you did not change (option a, agreed with the Exalted Items session)
				std::string   made;
				exporter::Tsv madeAgainst;
				if (job.sources.exalted.empty() || !ReadWhole(job.sources.exalted + L"\\" + Widen(std::string(folder) + "cubemain.txt"), made) || !exporter::ParseTsv(made, madeAgainst)) {
					madeAgainst = t;
				}
				return exporter::ApplyCubePlain(t, list, madeAgainst, report, main ? "cubemain" : "base\\cubemain");
			});
		}
	}
	if (!names.empty()) {
		std::string json;
		long        highest = 0;
		WIN32_FIND_DATAW entry {};
		const HANDLE     find = FindFirstFileW((stringsFolder + L"\\*.json").c_str(), &entry);
		if (find != INVALID_HANDLE_VALUE) {
			do {
				std::string text;
				if (ReadWhole(stringsFolder + L"\\" + entry.cFileName, text)) {
					const long h = exporter::HighestStringId(text);
					highest      = h > highest ? h : highest;
				}
			} while (FindNextFileW(find, &entry) != 0);
			FindClose(find);
		}
		const bool present = ReadWhole(stringsFolder + L"\\item-nameaffixes.json", json);
		if (!present) {
			json = gameAffixNames;
		}
		highest = exporter::HighestStringId(json) > highest ? exporter::HighestStringId(json) : highest;
		if (json.empty() || !WriteExport(stringsFolder + L"\\item-nameaffixes.json", exporter::AddStrings(json, names, highest + 1))) {
			report.problems.push_back("item-nameaffixes.json: the names of your own affixes could not be written");
		}
	}
	if (storage != nullptr) {
		CascCloseStorage(storage);
	}
	// modinfo.json: the new name, the same save folder (so the characters are there)
	std::string info, savepath;
	if (ReadWhole(job.sources.mod + L"\\modinfo.json", info)) {
		const size_t at = info.find("\"savepath\"");
		const size_t open = at == std::string::npos ? at : info.find('"', info.find(':', at) + 1);
		const size_t close = open == std::string::npos ? open : info.find('"', open + 1);
		if (close != std::string::npos) {
			savepath = info.substr(open + 1, close - open - 1);
		}
	}
	WriteExport(mpq + L"\\modinfo.json", "{\n  \"name\": \"" + job.name + "\",\n  \"savepath\": \"" + (savepath.empty() ? job.name + "/" : savepath) + "\"\n}\n");
	const std::string about = "Live Export " + std::string(kVersion) + ": " + job.name + ".mpq made from the mod '" + job.sources.modName + "'" + (withExalted ? " with Exalted Items' table copies" : " (plain: without Exalted Items' copies)")
	                        + "\r\n" + std::to_string(files) + " files of the mod" + (withExalted ? ", " + std::to_string(exaltedFiles) + " of Exalted Items" : "") + "\r\n\r\n" + ReportText(report)
	                        + (job.notes.empty() ? "" : "\r\nSkipped lines:\r\n" + job.notes)
	                        + "\r\nTo play it: copy the folder '" + job.name + "' into the game's mods folder and start the game with -mod " + job.name + ".\r\n"
	                        + (withExalted ? "It already holds Exalted Items' rows. Keep the Exalted Items plugin ON (version 1.1.0 or later): it sees that and uses the exported tables as they are, adding only its game hooks (unique and set drops, the restore strip, the loot filter box). An older Exalted Items would add its rows a second time.\r\n" : "");
	WriteExport(out + L"\\export-report.txt", about);
	LogF("%s", about.c_str());
	Tell({ "Live Export: " + job.name + ".mpq written (" + std::to_string(files + exaltedFiles) + " files, " + std::to_string(report.done.size()) + " changes in, " + std::to_string(report.problems.size()) + " left out)",
		"  " + Narrow(out) + "  (export-report.txt says what is in it)" });
}

void ExportD2rmm(Job& job) {
	const std::wstring out = job.sources.live + L"\\mod-export\\" + Widen(job.name) + L"-d2rmm";
	DeleteExport(out);
	HANDLE storage = nullptr;
	if (!CascOpenStorage(GameDirectory().c_str(), 0, &storage)) {
		storage = nullptr;
	}
	// the cube lists the recipes were made against: what the game read
	std::string   text;
	exporter::Tsv cube, cubeBase;
	if (ReadTable(job.sources, true, storage, "global\\excel\\cubemain.txt", text)) {
		exporter::ParseTsv(text, cube);
	}
	if (ReadTable(job.sources, true, storage, "global\\excel\\base\\cubemain.txt", text)) {
		exporter::ParseTsv(text, cubeBase);
	}
	std::set<std::string> keys = StringKeys(job.sources.mod + L"\\data\\local\\lng\\strings");
	std::string           names;
	if (ReadGame(storage, "local\\lng\\strings\\item-nameaffixes.json", names)) {
		for (size_t at = names.find("\"Key\""); at != std::string::npos; at = names.find("\"Key\"", at + 5)) {
			const size_t open = names.find('"', names.find(':', at) + 1), close = names.find('"', open + 1);
			keys.insert(names.substr(open + 1, close - open - 1));
		}
	}
	if (storage != nullptr) {
		CascCloseStorage(storage);
	}
	exporter::Report report;
	exporter::LeaveOutExaltedRows(job.edits, report); // D2RMM's tables never hold them; Exalted Items makes them at start
	const std::string ts   = exporter::D2rmmTs(job.edits, cube, cubeBase, [&](const std::string& w) { return keys.count(w) != 0; }, report);
	const std::string json = exporter::D2rmmJson(job.name, "1.0", "The changes made in the game with the Live Suite (made from the mod '" + job.sources.modName + "'). Install it after the mods it was made with.");
	const bool        ok   = WriteExport(out + L"\\mod.ts", ts) && WriteExport(out + L"\\mod.json", json);
	const std::string about = "Live Export " + std::string(kVersion) + ": the D2RMM mod '" + job.name + "' made from the mod '" + job.sources.modName + "'\r\n\r\n" + ReportText(report)
	                        + (job.notes.empty() ? "" : "\r\nSkipped lines:\r\n" + job.notes)
	                        + "\r\nTo use it: copy the folder '" + job.name + "-d2rmm' into D2RMM's mods folder (rename it as you like), put it after the mods it was made with, install.\r\n"
	                        + "A stat made in the game keeps its number: the mod fills the stat table with empty rows (live_unused_<n>) up to it, and stops if another mod already has a stat there.\r\n";
	WriteExport(out + L"\\export-report.txt", about);
	LogF("%s", about.c_str());
	Tell({ ok ? "Live Export: the D2RMM mod '" + job.name + "' written (" + std::to_string(report.done.size()) + " changes, " + std::to_string(report.problems.size()) + " left out)" : "Live Export: the D2RMM mod could not be written",
		"  " + Narrow(out) });
}

DWORD WINAPI Worker(void* data) {
	Job* job = static_cast<Job*>(data);
	if (job->d2rmm) {
		ExportD2rmm(*job);
	} else {
		ExportMpq(*job);
	}
	delete job;
	g_busy.store(false);
	return 0;
}

bool GoodName(const std::string& name) {
	if (name.empty() || name.size() > 40) {
		return false;
	}
	for (const char c : name) {
		if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
			return false;
		}
	}
	return name != "." && name.back() != ' ';
}

// Starts an export (the game's thread: the string numbers are asked of the game here).
void Start(bool d2rmm, bool plain, std::string name) {
	if (g_busy.load()) {
		Tell({ "Live Export: an export is still being written; wait for it." });
		return;
	}
	auto*       job = new Job;
	std::string problem;
	if (!FindSources(job->sources, problem)) {
		delete job;
		Tell({ "Live Export: " + problem });
		return;
	}
	if (name.empty()) {
		name = job->sources.modName + "-live";
	}
	if (!GoodName(name)) {
		delete job;
		Tell({ "Live Export: a name has letters, digits, spaces, - and _ only (40 at most)." });
		return;
	}
	job->d2rmm = d2rmm, job->plain = plain, job->name = name;
	job->notes = ReadEdits(job->sources, job->edits);
	MapStatStrings(job->sources, !plain, job->edits);
	if (job->edits.Empty() && d2rmm) {
		delete job;
		Tell({ "Live Export: there are no edits to export (nothing changed in the Live Suite's tabs, no recipe list)." });
		return;
	}
	g_busy.store(true);
	Tell({ "Live Export: writing " + std::string(d2rmm ? "the D2RMM mod '" + name + "'" : name + ".mpq (the whole mod, this takes a moment)") + "..." });
	const HANDLE thread = CreateThread(nullptr, 0, Worker, job, 0, nullptr);
	if (thread == nullptr) {
		g_busy.store(false);
		delete job;
		Tell({ "Live Export: could not start (error " + std::to_string(GetLastError()) + ")" });
		return;
	}
	CloseHandle(thread);
}

// ---------------------------------------------------------------- the console, the tab

void __cdecl SayJob(const D2RL::PluginContext*, void*) noexcept {
	std::vector<std::string> lines;
	AcquireSRWLockExclusive(&g_lock);
	lines.swap(g_say);
	ReleaseSRWLockExclusive(&g_lock);
	for (const std::string& line : lines) {
		g_ctx->WriteConsoleMessage(line.c_str());
	}
	if (g_book != nullptr && g_tab != 0) {
		g_book->redraw(g_tab);
	}
}

// Lines for the console and the tab, from any thread.
void Tell(const std::vector<std::string>& lines) {
	AcquireSRWLockExclusive(&g_lock);
	for (const std::string& line : lines) {
		g_say.push_back(line);
		LogF("%s", line.c_str());
	}
	g_status = lines;
	ReleaseSRWLockExclusive(&g_lock);
	if (g_threads != nullptr) {
		static_cast<void>(g_threads->runOnUiThread(g_ctx, SayJob, nullptr));
	}
}

constexpr char kUsage[] = "Live Export: export mpq [name] [plain] - the whole mod with your edits (plain: without Exalted Items' copies) | export d2rmm [name] - your edits as a D2RMM mod. "
                          "Written to d2rloader\\live-suite\\mod-export.";

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	std::vector<std::string> words;
	for (size_t at = 0; at < args.size();) {
		const size_t from = args.find_first_not_of(" \t", at);
		if (from == std::string::npos) {
			break;
		}
		const size_t end = args.find_first_of(" \t", from);
		words.push_back(args.substr(from, end == std::string::npos ? std::string::npos : end - from));
		at = end == std::string::npos ? args.size() : end;
	}
	if (words.empty() || (words[0] != "mpq" && words[0] != "d2rmm")) {
		g_ctx->WriteConsoleMessage(kUsage);
		return D2RL::ConsoleCommandResult::Handled;
	}
	const bool  plain = words.size() >= 2 && words.back() == "plain";
	std::string name;
	for (size_t i = 1; i < words.size() - (plain ? 1 : 0); ++i) {
		name += (name.empty() ? "" : " ") + words[i];
	}
	Start(words[0] == "d2rmm", plain, name);
	return D2RL::ConsoleCommandResult::Handled;
}

std::string g_typed; // the name typed in the tab (UI thread)

void __cdecl TabDraw(void*, modbook::Page* page) noexcept {
	const std::string mod = ModName();
	g_book->label(page, "About", 82, 150, 990, 100, "$FontColorGoldYellow", "left", 1,
		("Writes the mod '" + (mod.empty() ? std::string("(none)") : mod) + "' with your Live Suite edits (property, stat and affix rows, the cube list) into d2rloader\\live-suite\\mod-export.").c_str());
	g_book->label(page, "NameHead", 82, 300, 200, 40, "$FontColorGoldYellow", "left", 0, "Name:");
	g_book->image(page, "NameSlot", 210, 272, "PANEL\\Modals\\InputSlot");
	g_book->label(page, "Name", 250, 300, 660, 56, "$FontColorWhite", "left", 0, ((g_typed.empty() ? mod + "-live" : g_typed) + "_").c_str());
	g_book->button(page, "Mpq", 82, 420, 480, "The whole mod (.mpq)", "$FontColorGreen");
	g_book->button(page, "Plain", 592, 420, 480, ".mpq without Exalted copies", "$FontColorLightTeal");
	g_book->button(page, "D2rmm", 82, 500, 480, "A D2RMM mod (.ts + .json)", "$FontColorGreen");
	std::vector<std::string> status;
	AcquireSRWLockShared(&g_lock);
	status = g_status;
	ReleaseSRWLockShared(&g_lock);
	std::string text;
	for (const std::string& line : status) {
		text += line + "\n";
	}
	g_book->label(page, "Status", 82, 600, 990, 400, "$FontColorLightGold", "left", 1,
		(text.empty() ? std::string("Type a name (letters, digits, spaces, - and _), then a button. The .mpq is a copy of the whole mod with the edits written in, Exalted Items' table copies included unless you take the plain one. The D2RMM mod makes the same edits on top of other mods.") : text).c_str());
}
void __cdecl TabClick(void*, const char* what) noexcept {
	const std::string name = what != nullptr ? what : "";
	if (name == "Mpq" || name == "Plain" || name == "D2rmm") {
		Start(name == "D2rmm", name == "Plain", g_typed);
	} else if (name == "Escape" && g_book != nullptr) {
		g_book->close(modbook::SideOf(g_book, g_tab, modbook::kLeft));
	}
	if (g_book != nullptr) {
		g_book->redraw(g_tab);
	}
}
void __cdecl TabKeys(void*, const char* typed) noexcept {
	for (const char* at = typed; at != nullptr && *at != '\0'; ++at) {
		if (*at == modbook::kKeyBackspace) {
			if (!g_typed.empty()) {
				g_typed.pop_back();
			}
		} else if ((isalnum(static_cast<unsigned char>(*at)) || *at == '-' || *at == '_' || *at == ' ') && g_typed.size() < 40) {
			g_typed += *at;
		}
	}
	if (g_book != nullptr) {
		g_book->redraw(g_tab);
	}
}
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
	const modbook::Tab tab { sizeof(modbook::Tab), modbook::kLeft, "export", "Export", nullptr, TabDraw, TabClick, TabKeys, TabMove, TabShown, TabLeave, nullptr };
	g_tab = g_book->addTab(&tab);
	LogF("Mod Book: tab Export (left, %s)", g_tab != 0 ? "added" : "REFUSED, see mod-book.log");
}

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	BookSetUp();
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "live-export",
	.name        = "Live Export",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Writes the mod being played with the Live Suite's edits in it: a whole .mpq mod folder, or a D2RMM mod (.ts + .json). Only when asked.",
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
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
	}
	static_cast<void>(context->RegisterConsoleCommand("export", OnConsole, "Live Export: export mpq [name] [plain] | export d2rmm [name] - the mod with your Live Suite edits, to d2rloader\\live-suite\\mod-export"));
	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, registerGameplayEventListener))) {
		D2RL::Lifecycle::DataTablesLoadedListener tables {};
		tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
		tables.callback   = OnTablesLoaded;
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
	}
	BookSetUp(); // Mod Book may load later; then this happens when the tables are read
	LogF("Live Export %s: ready - console: export; Mod Book tab Export. It writes only when asked, to d2rloader\\live-suite\\mod-export", kVersion);
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	g_ctx = nullptr;
}
