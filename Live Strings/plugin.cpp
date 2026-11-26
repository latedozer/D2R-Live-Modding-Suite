// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Strings: text keys made while the game runs, through the loader's own string packages
// (live_strings_api.h says how). Every add / setText call = one new string file registered with the
// loader's ResourceService; the loader rebuilds its string tables at once. Nothing is unregistered.
//
// Kept: d2rloader\live-suite\live-strings\<mod>\strings.json (every revision), registered at plugin
// load as data/local/lng/strings/d2rloader/live-strings/kept.json.
//
// Console (Ctrl + `): strings | strings add <name> <text> | strings set <name> <text> |
//   strings get <name> | strings list | strings probe | strings probe swap

#include <D2RLPlugin/api.h>

#include "live_strings_api.h"
#include "strings_core.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.1.0";

const D2RL::PluginContext*       g_ctx       = nullptr;
const D2RL::ThreadService*       g_threads   = nullptr;
const D2RL::ResourceService*     g_resources = nullptr;
const D2RL::LocalizationService* g_loc       = nullptr;
HMODULE                          g_core      = nullptr;
bool                             g_enabled   = true;
std::string                      g_off;      // "" = on

struct View { const char* text; size_t length; };
using IdOrDefaultFn = uint16_t(__cdecl*)(const View* key, uint16_t fallback);
IdOrDefaultFn g_idOrDefault = nullptr;

// D2RCore's two-byte pool bitmap (65536 bits) at 0x7dc130, only read for poolUse() and only on the
// D2RCore build these bytes were taken from: the rebuild's "lea rcx,[0x7dc130]; mov r8d,0x2004".
constexpr uint32_t kKnownCoreStamp = 0x6ab37824;
constexpr uint32_t kPoolPrintRva   = 0x386deb;
constexpr uint8_t  kPoolPrint[13]  = {0x48, 0x8d, 0x0d, 0x3e, 0x53, 0x45, 0x00, 0x41, 0xb8, 0x04, 0x20, 0x00, 0x00};
constexpr uint32_t kPoolBitsRva    = 0x7dc130;
const uint64_t*    g_poolBits      = nullptr;

void LogF(const char* format, ...) {
	if (g_ctx == nullptr) return;
	char    text[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

void Say(const std::string& text) { if (g_ctx != nullptr) g_ctx->WriteConsoleMessage(("Live Strings: " + text).c_str()); }

std::wstring Widen(const std::string& s) {
	if (s.empty()) return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring& w) {
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

void LoadConfig() {
	static char text[2048];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) return;
	for (const char* line = text; *line != '\0';) {
		const char* end = strchr(line, '\n');
		const std::string one(line, end != nullptr ? static_cast<size_t>(end - line) : strlen(line));
		char key[32] = {}, value[32] = {};
		if (!one.empty() && one[0] != '#' && sscanf(one.c_str(), " %31[a-z_] = %31s", key, value) == 2 && !strcmp(key, "enabled")) g_enabled = strcmp(value, "false") != 0;
		if (end == nullptr) break;
		line = end + 1;
	}
}

// ---------------------------------------------------------------- files

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

std::wstring g_folder;
// d2rloader\live-suite\live-strings\<mod>: beside the plugins folder, never inside a mod's .mpq
std::wstring OwnFolder() {
	if (!g_folder.empty()) return g_folder;
	const std::string mod = g_ctx->activeMod != nullptr && g_ctx->activeMod[0] != '\0' ? g_ctx->activeMod : "no mod";
	g_folder = LoaderDir() + L"\\live-suite\\live-strings\\" + Widen(mod);
	return g_folder;
}
std::wstring KeptPath() { return OwnFolder() + L"\\strings.json"; }

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
	return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::string OwnSha256() {
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnSha256), &self)) return "?";
	wchar_t path[1024] = {};
	if (GetModuleFileNameW(self, path, 1000) == 0) return "?";
	std::string data;
	if (!ReadWhole(path, data)) return "?";
	BCRYPT_ALG_HANDLE  alg  = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	uint8_t            digest[32] = {};
	std::string        hex = "?";
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

// ---------------------------------------------------------------- the loader

const char* ResultName(D2RL::Resources::Result r) {
	switch (r) {
	case D2RL::Resources::Result::Success: return "Success";
	case D2RL::Resources::Result::InvalidArgument: return "InvalidArgument";
	case D2RL::Resources::Result::Unsupported: return "Unsupported";
	case D2RL::Resources::Result::Unavailable: return "Unavailable";
	case D2RL::Resources::Result::Conflict: return "Conflict (the loader refused the file and rebuilt without it; its log says why)";
	case D2RL::Resources::Result::NotFound: return "NotFound";
	case D2RL::Resources::Result::Busy: return "Busy";
	case D2RL::Resources::Result::OwnerInactive: return "OwnerInactive";
	case D2RL::Resources::Result::OwnerMismatch: return "OwnerMismatch";
	case D2RL::Resources::Result::StaleHandle: return "StaleHandle";
	case D2RL::Resources::Result::CallbackFault: return "CallbackFault";
	case D2RL::Resources::Result::TooLarge: return "TooLarge";
	}
	return "?";
}

double NowMs() {
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Registers one string file; the loader rebuilds its string tables inside this call when the plugin
// is active. ms = how long the call took (the hitch).
D2RL::Resources::Result Register(const std::string& file, const std::string& bytes, D2RL::Resources::RegistrationHandle& handle, double& ms) {
	const std::string path = ls::ResourcePath(file);
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.flags      = 0,
		.path       = path.c_str(),
		.bytes      = bytes.data(),
		.byteCount  = bytes.size(),
	};
	handle = D2RL::Resources::InvalidHandle;
	const double t0 = NowMs();
	const auto   r  = g_resources->registerResource(g_ctx, &resource, &handle);
	ms = NowMs() - t0;
	return r;
}

int NumberOf(const std::string& fullKey) {
	if (g_idOrDefault == nullptr) return -1;
	const View     v {fullKey.data(), fullKey.size()};
	const uint16_t n = g_idOrDefault(&v, 0xffff);
	return n == 0xffff ? -1 : n;
}

// The text the loader's localization service gives for one of our local keys (current language).
std::string ReadBack(const std::string& localKey) {
	if (g_loc == nullptr) return "(no localization service)";
	char     text[1024] = {};
	uint32_t need = 0;
	const auto r = g_loc->getStringByKey(g_ctx, localKey.c_str(), text, sizeof(text), &need);
	if (r == D2RL::Localization::Result::Success) return text;
	return "(not found, result " + std::to_string(static_cast<unsigned>(r)) + ")";
}

// ---------------------------------------------------------------- state

SRWLOCK  g_lock = SRWLOCK_INIT; // the bank, the queue, the listeners
ls::Bank g_bank;
int      g_seq = 0;             // add-<seq>.json this session

struct Job {
	bool                    isSet = false;
	std::vector<ls::Ask>    asks;
	livestrings::DoneFn     done = nullptr;
	void*                   user = nullptr;
};
std::deque<Job*> g_queue;

struct Listener { int handle; livestrings::ChangeFn fn; void* user; };
std::vector<Listener> g_listeners;
int                   g_nextListener = 1;

struct Lock {
	explicit Lock(bool exclusive = true) : x(exclusive) { x ? AcquireSRWLockExclusive(&g_lock) : AcquireSRWLockShared(&g_lock); }
	~Lock() { x ? ReleaseSRWLockExclusive(&g_lock) : ReleaseSRWLockShared(&g_lock); }
	bool x;
};

void Finish(const Job& job, int ok, const std::vector<ls::Record>& records, const std::vector<int>& numbers, const std::string& why, double ms) {
	if (job.done == nullptr) return;
	std::vector<std::string>            keys;
	std::vector<livestrings::KeyResult> results;
	keys.reserve(records.size());
	for (const ls::Record& r : records) keys.push_back(r.FullKey());
	for (size_t i = 0; i < records.size(); ++i)
		results.push_back(livestrings::KeyResult{records[i].name.c_str(), keys[i].c_str(), records[i].revision, i < numbers.size() ? numbers[i] : -1});
	const livestrings::Done d {sizeof(livestrings::Done), ok, static_cast<uint32_t>(ok ? results.size() : 0), ok ? results.data() : nullptr, why.c_str(), ms};
	job.done(job.user, &d);
}

// One queued call: plan, one file, one registration (= one rebuild), keep, numbers, tell.
void __cdecl RunJob(const D2RL::PluginContext*, void*) noexcept {
	Job* job = nullptr;
	std::vector<ls::Record> records;
	std::string             why;
	bool                    planned = false;
	int                     seq = 0;
	{
		Lock lock;
		if (g_queue.empty()) return;
		job = g_queue.front();
		g_queue.pop_front();
		planned = g_off.empty() ? g_bank.Plan(job->asks, job->isSet, records, why) : (why = g_off, false);
		if (planned) seq = ++g_seq;
	}
	if (!planned) {
		LogF("%s refused: %s", job->isSet ? "setText" : "add", why.c_str());
		Finish(*job, 0, {}, {}, why, 0);
		delete job;
		return;
	}
	const std::string file  = "add-" + std::to_string(seq) + ".json";
	const std::string bytes = ls::FileJson(records);
	D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
	double ms = 0;
	const auto r = Register(file, bytes, handle, ms);
	if (r != D2RL::Resources::Result::Success) {
		why = std::string("the loader did not take ") + file + ": " + ResultName(r);
		LogF("%s: %s (%.1f ms)", job->isSet ? "setText" : "add", why.c_str(), ms);
		Finish(*job, 0, {}, {}, why, ms);
		delete job;
		return;
	}
	std::string kept;
	{
		Lock lock;
		g_bank.Commit(records);
		kept = g_bank.KeptJson();
	}
	const bool saved = WriteOwn(KeptPath(), kept);
	std::vector<int> numbers;
	std::string      list;
	for (const ls::Record& rec : records) {
		numbers.push_back(NumberOf(rec.FullKey()));
		list += (list.empty() ? "" : ", ") + rec.LocalKey() + "=" + std::to_string(numbers.back());
	}
	LogF("%s: %zu key(s) in %s (%zu bytes), rebuild %.1f ms: %s%s", job->isSet ? "setText" : "add", records.size(), file.c_str(), bytes.size(), ms, list.c_str(),
	     saved ? "" : ". WARNING: strings.json could not be written - these keys are gone at the next start");
	std::vector<Listener> listeners;
	{
		Lock lock(false);
		listeners = g_listeners;
	}
	for (size_t i = 0; i < records.size(); ++i) {
		const std::string key = records[i].FullKey();
		const livestrings::Change c {sizeof(livestrings::Change), job->isSet ? livestrings::ChangeKind::TextChanged : livestrings::ChangeKind::Added,
		                             records[i].name.c_str(), key.c_str(), records[i].revision, numbers[i]};
		for (const Listener& l : listeners) l.fn(l.user, &c);
	}
	Finish(*job, 1, records, numbers, saved ? "" : "strings.json could not be written: gone at the next start", ms);
	delete job;
}

int Queue(const livestrings::Text* items, size_t count, bool isSet, livestrings::DoneFn done, void* user) noexcept {
	if (items == nullptr || count == 0 || count > 4096 || g_threads == nullptr || !g_off.empty()) return 0;
	Job* job = new Job;
	job->isSet = isSet;
	job->done  = done;
	job->user  = user;
	for (size_t i = 0; i < count; ++i) {
		ls::Ask a;
		a.name = items[i].name != nullptr ? items[i].name : "";
		for (int k = 0; k < ls::kLanguages; ++k) {
			a.has[k]  = items[i].lang[k] != nullptr;
			a.lang[k] = a.has[k] ? items[i].lang[k] : "";
		}
		job->asks.push_back(a);
	}
	{
		Lock lock;
		g_queue.push_back(job);
	}
	if (g_threads->runOnUiThread(g_ctx, RunJob, nullptr) != D2RL::Threads::Result::Success) {
		Lock lock;
		for (auto it = g_queue.begin(); it != g_queue.end(); ++it) if (*it == job) { g_queue.erase(it); break; }
		delete job;
		return 0;
	}
	return 1;
}

// ---------------------------------------------------------------- API

int __cdecl ApiAdd(const livestrings::Text* items, size_t count, livestrings::DoneFn done, void* user) noexcept { return Queue(items, count, false, done, user); }
int __cdecl ApiSetText(const livestrings::Text* items, size_t count, livestrings::DoneFn done, void* user) noexcept { return Queue(items, count, true, done, user); }

bool Latest(const char* name, ls::Record& out) {
	if (name == nullptr) return false;
	Lock lock(false);
	const ls::Bank::Entry* e = g_bank.Find(name);
	if (e == nullptr || e->revisions.empty()) return false;
	out = ls::Record{e->name, static_cast<int>(e->revisions.size()), e->revisions.back()};
	return true;
}

int __cdecl ApiNumber(const char* name) noexcept {
	ls::Record r;
	return Latest(name, r) ? NumberOf(r.FullKey()) : -1;
}

int __cdecl ApiKey(const char* name, char* out, size_t size) noexcept {
	ls::Record r;
	if (!Latest(name, r)) return 0;
	if (out != nullptr && size > 0) snprintf(out, size, "%s", r.FullKey().c_str());
	return r.revision;
}

int __cdecl ApiText(const char* name, int language, char* out, size_t size) noexcept {
	ls::Record r;
	if (language < 0 || language >= ls::kLanguages || !Latest(name, r)) return 0;
	if (out != nullptr && size > 0) snprintf(out, size, "%s", r.texts.lang[language].c_str());
	return 1;
}

int __cdecl ApiList(livestrings::ListFn callback, void* user) noexcept {
	std::vector<ls::Bank::Entry> copy;
	{
		Lock lock(false);
		copy = g_bank.Entries();
	}
	int n = 0;
	for (const ls::Bank::Entry& e : copy) {
		for (size_t k = 0; k < e.revisions.size(); ++k) {
			const ls::Record   r {e.name, static_cast<int>(k) + 1, e.revisions[k]};
			const std::string  full = r.FullKey(), local = r.LocalKey();
			livestrings::Entry x {};
			x.size     = sizeof(x);
			x.name     = e.name.c_str();
			x.key      = full.c_str();
			x.localKey = local.c_str();
			x.revision = r.revision;
			x.latest   = k + 1 == e.revisions.size() ? 1 : 0;
			for (int i = 0; i < ls::kLanguages; ++i) x.text[i] = e.revisions[k].lang[i].c_str();
			if (callback != nullptr) callback(user, &x);
			++n;
		}
	}
	return n;
}

int __cdecl ApiPoolUse(uint32_t* used, uint32_t* most) noexcept {
	if (g_poolBits == nullptr) return 0;
	uint32_t n = 0;
	for (int i = 0; i < 1024; ++i) n += static_cast<uint32_t>(__builtin_popcountll(g_poolBits[i]));
	if (used != nullptr) *used = n;
	if (most != nullptr) *most = 0xffff;
	return 1;
}

int __cdecl ApiStatus(char* text, size_t size) noexcept {
	if (text != nullptr && size > 0) snprintf(text, size, "%s", g_off.empty() ? "on" : g_off.c_str());
	return g_off.empty() ? 1 : 0;
}

int __cdecl ApiAddListener(livestrings::ChangeFn callback, void* user) noexcept {
	if (callback == nullptr) return 0;
	Lock lock;
	g_listeners.push_back(Listener{g_nextListener, callback, user});
	return g_nextListener++;
}

void __cdecl ApiRemoveListener(int handle) noexcept {
	Lock lock;
	for (auto it = g_listeners.begin(); it != g_listeners.end(); ++it) if (it->handle == handle) { g_listeners.erase(it); return; }
}

const livestrings::Api g_api {
	sizeof(livestrings::Api), livestrings::kApiVersion,
	ApiAdd, ApiSetText, ApiNumber, ApiKey, ApiText, ApiList, ApiPoolUse, ApiStatus, ApiAddListener, ApiRemoveListener,
};

// ---------------------------------------------------------------- the override probe
// Question: can the text of a key change IN PLACE (same key, same two-byte number) with a string
// override from this plugin? D2RCore's catalog builder (0x4c690) says an override's conflict key is
// (target key, language, package class), so a second override of one key should be refused, and
// changing it would need: unregister the first override (the key itself stays defined by its own
// file), then register the next. Only registrations of new files and the unregistering of an
// OVERRIDE file are done here; a refused file is rolled back by the loader. Session only: nothing is
// kept, the probe key never reaches a table.
struct Probe {
	bool base = false, over1 = false, over2 = false, swapped = false;
	D2RL::Resources::RegistrationHandle h1 = D2RL::Resources::InvalidHandle, h2 = D2RL::Resources::InvalidHandle;
} g_probe;

ls::Record ProbeRecord(const std::string& text) {
	ls::Record r;
	r.name     = "probe";
	r.revision = 1;
	for (int i = 0; i < ls::kLanguages; ++i) r.texts.lang[i] = text;
	return r;
}

void Report(const char* format, ...) {
	char    text[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	LogF("probe: %s", text);
	Say(std::string("probe: ") + text);
}

void __cdecl ProbeJob(const D2RL::PluginContext*, void* swap) noexcept {
	const std::string key = ProbeRecord("").LocalKey(); // probe_r1
	double ms = 0;
	D2RL::Resources::RegistrationHandle h = D2RL::Resources::InvalidHandle;
	if (swap == nullptr) {
		if (!g_probe.base) {
			const auto r = Register("probe-base.json", ls::FileJson({ProbeRecord("Live Strings probe: base text")}), h, ms);
			g_probe.base = r == D2RL::Resources::Result::Success;
			Report("1 base key %s: %s, rebuild %.1f ms, reads \"%s\"", key.c_str(), ResultName(r), ms, ReadBack(key).c_str());
			if (!g_probe.base) return;
		}
		if (!g_probe.over1) {
			const auto r = Register("probe-override-1.json", ls::FileJson({ProbeRecord("Live Strings probe: override 1")}, "live-strings:"), g_probe.h1, ms);
			g_probe.over1 = r == D2RL::Resources::Result::Success;
			Report("2 own override 1 of live-strings:%s: %s, rebuild %.1f ms, reads \"%s\" (override 1 = a plugin may override its own key)", key.c_str(), ResultName(r), ms, ReadBack(key).c_str());
			if (!g_probe.over1) { Report("RESULT: a plugin may NOT override its own key; setText stays one new revision per edit"); return; }
		}
		if (!g_probe.over2) {
			const auto r = Register("probe-override-2.json", ls::FileJson({ProbeRecord("Live Strings probe: override 2")}, "live-strings:"), g_probe.h2, ms);
			g_probe.over2 = r == D2RL::Resources::Result::Success;
			Report("3 second own override of the same key: %s, rebuild %.1f ms, reads \"%s\" (expected: refused)", ResultName(r), ms, ReadBack(key).c_str());
		}
		Report("next: strings probe swap (unregisters override 1 - the key stays defined by probe-base.json - then registers override 2)");
		return;
	}
	if (!g_probe.over1 || g_probe.over2 || g_probe.swapped) {
		Report("swap needs steps 1-3 first with override 1 taken and override 2 refused (now: override1 %d, override2 %d, swapped %d)", g_probe.over1, g_probe.over2, g_probe.swapped);
		return;
	}
	const int number = NumberOf(std::string("live-strings:") + key); // a number makes it a real case: D2RCore re-binds numbered keys at every rebuild
	Report("4 the probe key now holds two-byte number %d", number);
	double t0 = NowMs();
	const auto u = g_resources->unregisterResource(g_ctx, g_probe.h1);
	ms = NowMs() - t0;
	Report("5 unregister override 1: %s, rebuild %.1f ms, reads \"%s\" (expected: the base text)", ResultName(u), ms, ReadBack(key).c_str());
	if (u != D2RL::Resources::Result::Success) return;
	g_probe.over1 = false;
	const auto r = Register("probe-override-2.json", ls::FileJson({ProbeRecord("Live Strings probe: override 2")}, "live-strings:"), g_probe.h2, ms);
	g_probe.over2   = r == D2RL::Resources::Result::Success;
	g_probe.swapped = true;
	const int after = NumberOf(std::string("live-strings:") + key);
	Report("6 register override 2: %s, rebuild %.1f ms, reads \"%s\", number %d (was %d)", ResultName(r), ms, ReadBack(key).c_str(), after, number);
	Report("RESULT: %s", g_probe.over2 && after == number ? "own-key overrides swap in place (same number): setText could keep one key" : "the swap did not work: setText stays one new revision per edit");
}

// ---------------------------------------------------------------- console

void __cdecl ConsoleDone(void*, const livestrings::Done* d) noexcept {
	if (!d->ok) { Say(std::string("refused: ") + d->why); return; }
	for (uint32_t i = 0; i < d->count; ++i)
		Say(std::string(d->keys[i].key) + " = number " + std::to_string(d->keys[i].number) + " (rebuild " + std::to_string(static_cast<int>(d->hitchMs)) + " ms)");
}

void __cdecl ConsoleList(void*, const livestrings::Entry* e) noexcept {
	Say(std::string(e->key) + (e->latest ? " (latest): " : ": ") + e->text[0]);
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* context, void*) noexcept {
	const std::string args(context != nullptr && context->args != nullptr ? context->args : "", context != nullptr && context->args != nullptr ? context->argsLength : 0);
	char what[16] = {}, name[64] = {};
	int  used = 0;
	if (!g_off.empty()) { Say("off - " + g_off); return D2RL::ConsoleCommandResult::Handled; }
	if (sscanf(args.c_str(), " %15s%n", what, &used) == 1) {
		if (!strcmp(what, "add") || !strcmp(what, "set")) {
			int more = 0;
			if (sscanf(args.c_str() + used, " %63s %n", name, &more) < 1 || more == 0) { Say("strings add|set <name> <enUS text>"); return D2RL::ConsoleCommandResult::Handled; }
			const std::string text = args.substr(static_cast<size_t>(used + more));
			livestrings::Text t {};
			t.name    = name;
			t.lang[0] = text.c_str();
			if (!Queue(&t, 1, !strcmp(what, "set"), ConsoleDone, nullptr)) Say("could not queue it");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (!strcmp(what, "get") && sscanf(args.c_str() + used, " %63s", name) == 1) {
			char key[128] = {};
			const int rev = ApiKey(name, key, sizeof(key));
			if (rev == 0) { Say(std::string("no name ") + name); return D2RL::ConsoleCommandResult::Handled; }
			ls::Record r;
			Latest(name, r);
			Say(std::string(key) + " = number " + std::to_string(NumberOf(key)) + ", reads \"" + ReadBack(r.LocalKey()) + "\"");
			return D2RL::ConsoleCommandResult::Handled;
		}
		if (!strcmp(what, "list")) { if (ApiList(ConsoleList, nullptr) == 0) Say("no names yet"); return D2RL::ConsoleCommandResult::Handled; }
		if (!strcmp(what, "probe")) {
			char sub[16] = {};
			const bool swap = sscanf(args.c_str() + used, " %15s", sub) == 1 && !strcmp(sub, "swap");
			if (g_threads->runOnUiThread(g_ctx, ProbeJob, swap ? reinterpret_cast<void*>(1) : nullptr) != D2RL::Threads::Result::Success) Say("could not queue the probe");
			return D2RL::ConsoleCommandResult::Handled;
		}
	}
	size_t names = 0, revisions = 0;
	{
		Lock lock(false);
		names     = g_bank.Entries().size();
		revisions = g_bank.Revisions();
	}
	uint32_t usedNumbers = 0, most = 0;
	const std::string pool = ApiPoolUse(&usedNumbers, &most) ? std::to_string(usedNumbers) + " of " + std::to_string(most) + " two-byte numbers in use" : "pool not readable on this D2RCore";
	Say(std::to_string(names) + " names, " + std::to_string(revisions) + " revisions (kept in " + Narrow(KeptPath()) + "); " + pool);
	Say("strings add|set <name> <enUS text> | get <name> | list | probe [swap]");
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------- load

// The kept revisions, given to the loader while this plugin loads (staged: they are part of the
// string tables the game builds at start, before any table names them).
void LoadKept() {
	std::string text;
	if (!ReadWhole(KeptPath(), text)) { LogF("kept: none yet (%ls)", KeptPath().c_str()); return; }
	std::string why;
	ls::Bank    bank;
	if (!bank.LoadKept(text, why)) {
		g_off = "the kept file " + Narrow(KeptPath()) + " cannot be read (" + why + "); it is left as it is and nothing is added this start";
		g_ctx->LogError(("Live Strings: " + g_off).c_str());
		return;
	}
	if (bank.Entries().empty()) return;
	const std::string bytes = bank.KeptJson();
	D2RL::Resources::RegistrationHandle h = D2RL::Resources::InvalidHandle;
	double ms = 0;
	const auto r = Register("kept.json", bytes, h, ms);
	if (r != D2RL::Resources::Result::Success) {
		g_off = std::string("the loader did not take kept.json: ") + ResultName(r) + "; nothing is added this start";
		g_ctx->LogError(("Live Strings: " + g_off).c_str());
		return;
	}
	g_bank = bank;
	LogF("kept: %zu names, %zu revisions registered as kept.json (%zu bytes)", g_bank.Entries().size(), g_bank.Revisions(), bytes.size());
}

void LinkCore() {
	g_core = GetModuleHandleW(L"D2RCore.dll");
	if (g_core == nullptr) { g_off = "D2RCore.dll is not loaded"; return; }
	g_idOrDefault = reinterpret_cast<IdOrDefaultFn>(reinterpret_cast<void*>(GetProcAddress(g_core, "GetNamespacedStringIdOrDefault")));
	if (g_idOrDefault == nullptr) { g_off = "D2RCore.dll has no GetNamespacedStringIdOrDefault"; return; }
	const auto* base  = reinterpret_cast<const uint8_t*>(g_core);
	const auto* dos   = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt    = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	const uint32_t st = nt->FileHeader.TimeDateStamp;
	if (st == kKnownCoreStamp && nt->OptionalHeader.SizeOfImage > kPoolBitsRva + 0x2000 && memcmp(base + kPoolPrintRva, kPoolPrint, sizeof(kPoolPrint)) == 0)
		g_poolBits = reinterpret_cast<const uint64_t*>(base + kPoolBitsRva);
	LogF("D2RCore stamp %08x: pool count %s", st, g_poolBits != nullptr ? "readable" : "not readable (other D2RCore build); everything else works");
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "live-strings",
	.name        = "Live Strings",
	.version     = kVersion,
	.author      = "Latedozer",
	.description = "Text keys made while the game runs, through the loader's own string packages.",
	.flags       = D2RL::PluginFlags::Shared,
	.reserved    = {},
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* { return &kPluginInfo; }

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) return false;
	g_ctx = context;
	LoadConfig();
	LogF("build: Live Strings %s, d2rl-live-strings.dll sha256 %s, API version %u", kVersion, OwnSha256().c_str(), livestrings::kApiVersion);
	static_cast<void>(context->RegisterConsoleCommand("strings", OnConsole, "Live Strings: text keys made during play; strings add|set|get|list|probe"));
	if (!g_enabled) { g_off = "off in the settings (enabled = false)"; LogF("Live Strings %s: %s; kept texts do NOT reach the game", kVersion, g_off.c_str()); return true; }
	if (context->QueryService(&g_resources) != D2RL::ServiceQueryResult::Success || !D2RL::HasResourceServiceField(g_resources, D2RL::ResourceServiceRequiredSize)) {
		g_resources = nullptr;
		g_off = "this D2RLoader has no resource service";
	} else if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
		g_threads = nullptr;
		g_off = "this D2RLoader has no thread service";
	}
	if (context->QueryService(&g_loc) != D2RL::ServiceQueryResult::Success || !D2RL::HasLocalizationServiceField(g_loc, D2RL::LocalizationServiceRequiredSize)) g_loc = nullptr;
	if (g_resources != nullptr) {
		const std::string was = g_off;
		g_off.clear();
		LoadKept();   // kept texts reach the game whenever the loader can take a file
		if (g_off.empty()) g_off = was;
	}
	if (g_off.empty()) LinkCore();
	HMODULE self = nullptr; // stay loaded: other plugins hold our API table
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&RunJob), &self);
	if (g_off.empty()) LogF("Live Strings %s: on - type strings in the console", kVersion);
	else LogF("Live Strings %s: adding is OFF - %s", kVersion, g_off.c_str());
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}

D2RL_PLUGIN_EXPORT const livestrings::Api* __cdecl LiveStringsGetApi(uint32_t version) noexcept {
	return version >= 1 ? &g_api : nullptr;
}
