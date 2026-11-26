// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Live Strings' core, with no Windows or game calls (the plugin and the native test both use it):
// names and their revisions, the string files D2RCore reads (JSON arrays of {"Key", <languages>}),
// the kept file and how it is read back at load.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ls {

constexpr int    kLanguages   = 13;
constexpr size_t kMaxName     = 48;
constexpr size_t kMaxText     = 4096;
constexpr int    kMaxRevision = 9999;
extern const char* const kCodes[kLanguages]; // enUS zhTW deDE esES frFR itIT koKR plPL esMX jaJP ptBR ruRU zhCN
constexpr const char kNamespace[] = "live-strings";

struct Texts {
	std::string lang[kLanguages];
};

struct Record { // one key as written to a string file
	std::string name;
	int         revision = 0;
	Texts       texts;
	std::string LocalKey() const; // <name>_r<revision>
	std::string FullKey() const;  // live-strings:<name>_r<revision>
};

struct Ask { // one item of an add / setText call; lang[i] empty and has[i] false = copy of enUS
	std::string name;
	std::string lang[kLanguages];
	bool        has[kLanguages] = {};
};

bool ValidName(const std::string& name, std::string& why);
bool ValidText(const std::string& text, std::string& why);
// "<name>_r<k>" -> name, k; false when it does not end in _r<digits> or the name is not valid
bool SplitKey(const std::string& localKey, std::string& name, int& revision);

class Bank {
public:
	struct Entry {
		std::string        name;
		std::vector<Texts> revisions; // [0] = revision 1
	};
	// The records an add (isSet false) or setText call would write, or false and why (nothing changes).
	bool Plan(const std::vector<Ask>& asks, bool isSet, std::vector<Record>& out, std::string& why) const;
	// After the loader took the file: the records become part of the bank.
	void Commit(const std::vector<Record>& records);
	const Entry* Find(const std::string& name) const;
	const std::vector<Entry>& Entries() const { return entries_; }
	size_t Revisions() const;
	// Every revision of every name, as one string file (the kept file and what is registered at load).
	std::string KeptJson() const;
	// Read a kept file back; false and why on anything that is not a file this writes (the bank is then empty).
	bool LoadKept(const std::string& json, std::string& why);
	void Clear() { entries_.clear(); }

private:
	std::vector<Entry> entries_;
};

// A string file D2RCore reads: [ {"Key": "...", "enUS": "...", ...}, ... ] (no id field: namespaced).
// keyPrefix "" = local keys (<name>_r<k>), else e.g. "live-strings:" for an override of own keys.
std::string FileJson(const std::vector<Record>& records, const std::string& keyPrefix = "");
std::string Escape(const std::string& text); // JSON string body

// Minimal reader for a JSON array of objects whose values are strings (what FileJson writes, any
// whitespace, a UTF-8 BOM, \uXXXX with surrogate pairs). Other value types are an error.
struct Object {
	std::vector<std::pair<std::string, std::string>> fields;
	const std::string* Get(const std::string& field) const;
};
bool ParseArray(const std::string& json, std::vector<Object>& out, std::string& why);

// The resource path of a session file (data/local/lng/strings/d2rloader/live-strings/<file>).
std::string ResourcePath(const std::string& file);

} // namespace ls
