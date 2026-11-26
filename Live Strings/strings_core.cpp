// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "strings_core.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace ls {

const char* const kCodes[kLanguages] = {"enUS", "zhTW", "deDE", "esES", "frFR", "itIT", "koKR",
                                        "plPL", "esMX", "jaJP", "ptBR", "ruRU", "zhCN"};

std::string Record::LocalKey() const { return name + "_r" + std::to_string(revision); }
std::string Record::FullKey() const { return std::string(kNamespace) + ":" + LocalKey(); }

bool ValidName(const std::string& name, std::string& why) {
	if (name.empty() || name.size() > kMaxName) { why = "a name is 1-48 letters, digits or _ (\"" + name + "\")"; return false; }
	for (const char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) { why = "a name is 1-48 letters, digits or _ (\"" + name + "\")"; return false; }
	}
	return true;
}

bool ValidText(const std::string& text, std::string& why) {
	if (text.size() > kMaxText) { why = "a text is at most 4096 bytes"; return false; }
	if (text.find('\0') != std::string::npos) { why = "a text cannot hold a NUL byte"; return false; }
	// UTF-8 shape (D2RCore's reader refuses invalid UTF-8 and then the WHOLE file is refused)
	for (size_t i = 0; i < text.size();) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (n == 0 || i + n > text.size()) { why = "a text is not valid UTF-8"; return false; }
		for (size_t k = 1; k < n; ++k)
			if ((static_cast<unsigned char>(text[i + k]) >> 6) != 2) { why = "a text is not valid UTF-8"; return false; }
		i += n;
	}
	return true;
}

bool SplitKey(const std::string& localKey, std::string& name, int& revision) {
	const size_t at = localKey.rfind("_r");
	if (at == std::string::npos || at == 0 || at + 2 >= localKey.size() || localKey.size() - at - 2 > 4) return false;
	int k = 0;
	for (size_t i = at + 2; i < localKey.size(); ++i) {
		if (localKey[i] < '0' || localKey[i] > '9') return false;
		k = k * 10 + (localKey[i] - '0');
	}
	if (k < 1 || localKey[at + 2] == '0') return false;
	name = localKey.substr(0, at);
	std::string why;
	if (!ValidName(name, why)) return false;
	revision = k;
	return true;
}

const Bank::Entry* Bank::Find(const std::string& name) const {
	for (const Entry& e : entries_) if (e.name == name) return &e;
	return nullptr;
}

size_t Bank::Revisions() const {
	size_t n = 0;
	for (const Entry& e : entries_) n += e.revisions.size();
	return n;
}

bool Bank::Plan(const std::vector<Ask>& asks, bool isSet, std::vector<Record>& out, std::string& why) const {
	out.clear();
	if (asks.empty()) { why = "nothing to do"; return false; }
	std::set<std::string> seen;
	for (const Ask& a : asks) {
		if (!ValidName(a.name, why)) return false;
		if (!seen.insert(a.name).second) { why = "\"" + a.name + "\" is twice in one call"; return false; }
		Record r;
		r.name = a.name;
		const Entry* e = Find(a.name);
		if (isSet) {
			if (e == nullptr) { why = "\"" + a.name + "\" was never added (add it first)"; return false; }
			if (static_cast<int>(e->revisions.size()) >= kMaxRevision) { why = "\"" + a.name + "\" has 9999 revisions"; return false; }
			r.revision = static_cast<int>(e->revisions.size()) + 1;
		} else {
			if (e != nullptr) { why = "\"" + a.name + "\" is already added (use setText for new text)"; return false; }
			r.revision = 1;
		}
		for (int i = 0; i < kLanguages; ++i) {
			r.texts.lang[i] = (i == 0 || a.has[i]) ? a.lang[i] : a.lang[0];
			if (!ValidText(r.texts.lang[i], why)) { why = "\"" + a.name + "\" " + kCodes[i] + ": " + why; return false; }
		}
		out.push_back(r);
	}
	return true;
}

void Bank::Commit(const std::vector<Record>& records) {
	for (const Record& r : records) {
		Entry* e = nullptr;
		for (Entry& x : entries_) if (x.name == r.name) e = &x;
		if (e == nullptr) { entries_.push_back(Entry{r.name, {}}); e = &entries_.back(); }
		if (static_cast<int>(e->revisions.size()) + 1 == r.revision) e->revisions.push_back(r.texts);
	}
}

std::string Escape(const std::string& text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (const char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
			else out += ch;
		}
	}
	return out;
}

std::string FileJson(const std::vector<Record>& records, const std::string& keyPrefix) {
	std::string out = "[\n";
	for (size_t n = 0; n < records.size(); ++n) {
		const Record& r = records[n];
		out += "  {\"Key\": \"" + Escape(keyPrefix + r.LocalKey()) + "\"";
		for (int i = 0; i < kLanguages; ++i) out += std::string(", \"") + kCodes[i] + "\": \"" + Escape(r.texts.lang[i]) + "\"";
		out += n + 1 < records.size() ? "},\n" : "}\n";
	}
	return out + "]\n";
}

std::string Bank::KeptJson() const {
	std::vector<Record> all;
	for (const Entry& e : entries_)
		for (size_t k = 0; k < e.revisions.size(); ++k) all.push_back(Record{e.name, static_cast<int>(k) + 1, e.revisions[k]});
	return FileJson(all);
}

const std::string* Object::Get(const std::string& field) const {
	for (const auto& f : fields) if (f.first == field) return &f.second;
	return nullptr;
}

namespace {

struct Reader {
	const std::string& s;
	size_t             i = 0;
	std::string        why;
	void Space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
	bool Fail(const char* what) { if (why.empty()) why = std::string(what) + " at byte " + std::to_string(i); return false; }
	static void Utf8(uint32_t cp, std::string& out) {
		if (cp < 0x80) out += static_cast<char>(cp);
		else if (cp < 0x800) { out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3f)); }
		else if (cp < 0x10000) { out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f)); out += static_cast<char>(0x80 | (cp & 0x3f)); }
		else { out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f)); out += static_cast<char>(0x80 | (cp & 0x3f)); }
	}
	bool Hex4(uint32_t& v) {
		if (i + 4 > s.size()) return Fail("short \\u escape");
		v = 0;
		for (int k = 0; k < 4; ++k) {
			const char c = s[i++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return Fail("bad \\u escape");
		}
		return true;
	}
	bool String(std::string& out) {
		out.clear();
		if (i >= s.size() || s[i] != '"') return Fail("expected a string");
		++i;
		while (i < s.size()) {
			const char c = s[i++];
			if (c == '"') return true;
			if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in a string");
			if (c != '\\') { out += c; continue; }
			if (i >= s.size()) break;
			const char e = s[i++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				uint32_t v = 0;
				if (!Hex4(v)) return false;
				if (v >= 0xd800 && v < 0xdc00) {
					uint32_t lo = 0;
					if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return Fail("lone surrogate");
					i += 2;
					if (!Hex4(lo) || lo < 0xdc00 || lo >= 0xe000) return Fail("bad surrogate pair");
					v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
				} else if (v >= 0xdc00 && v < 0xe000) return Fail("lone surrogate");
				Utf8(v, out);
				break;
			}
			default: return Fail("bad escape");
			}
		}
		return Fail("unclosed string");
	}
};

} // namespace

bool ParseArray(const std::string& json, std::vector<Object>& out, std::string& why) {
	out.clear();
	Reader r{json, 0, std::string()};
	if (json.compare(0, 3, "\xef\xbb\xbf") == 0) r.i = 3;
	r.Space();
	if (r.i >= json.size() || json[r.i] != '[') { r.Fail("expected ["); why = r.why; return false; }
	++r.i;
	r.Space();
	if (r.i < json.size() && json[r.i] == ']') { ++r.i; r.Space(); if (r.i != json.size()) { r.Fail("text after ]"); why = r.why; return false; } return true; }
	while (true) {
		r.Space();
		if (r.i >= json.size() || json[r.i] != '{') { r.Fail("expected {"); why = r.why; return false; }
		++r.i;
		Object o;
		r.Space();
		if (r.i < json.size() && json[r.i] == '}') ++r.i;
		else while (true) {
			r.Space();
			std::string k, v;
			if (!r.String(k)) { why = r.why; return false; }
			r.Space();
			if (r.i >= json.size() || json[r.i] != ':') { r.Fail("expected :"); why = r.why; return false; }
			++r.i;
			r.Space();
			if (!r.String(v)) { why = r.why; return false; }
			for (const auto& f : o.fields) if (f.first == k) { r.Fail(("duplicate field " + k).c_str()); why = r.why; return false; }
			o.fields.emplace_back(k, v);
			r.Space();
			if (r.i < json.size() && json[r.i] == ',') { ++r.i; continue; }
			if (r.i < json.size() && json[r.i] == '}') { ++r.i; break; }
			r.Fail("expected , or }"); why = r.why; return false;
		}
		out.push_back(std::move(o));
		r.Space();
		if (r.i < json.size() && json[r.i] == ',') { ++r.i; continue; }
		if (r.i < json.size() && json[r.i] == ']') { ++r.i; break; }
		r.Fail("expected , or ]"); why = r.why; return false;
	}
	r.Space();
	if (r.i != json.size()) { r.Fail("text after ]"); why = r.why; return false; }
	return true;
}

bool Bank::LoadKept(const std::string& json, std::string& why) {
	entries_.clear();
	std::vector<Object> objects;
	if (!ParseArray(json, objects, why)) return false;
	for (size_t n = 0; n < objects.size(); ++n) {
		const Object& o = objects[n];
		const std::string* key = o.Get("Key");
		std::string name;
		int k = 0;
		if (key == nullptr || !SplitKey(*key, name, k)) { why = "record " + std::to_string(n + 1) + ": no Key <name>_r<revision>"; entries_.clear(); return false; }
		Record r;
		r.name = name;
		r.revision = k;
		for (int i = 0; i < kLanguages; ++i) {
			const std::string* t = o.Get(kCodes[i]);
			if (t == nullptr) { why = *key + ": no " + kCodes[i]; entries_.clear(); return false; }
			if (!ValidText(*t, why)) { why = *key + " " + kCodes[i] + ": " + why; entries_.clear(); return false; }
			r.texts.lang[i] = *t;
		}
		const Entry* e = Find(name);
		const int have = e != nullptr ? static_cast<int>(e->revisions.size()) : 0;
		if (k != have + 1) { why = *key + ": revisions out of order (expected _r" + std::to_string(have + 1) + ")"; entries_.clear(); return false; }
		Commit({r});
	}
	return true;
}

std::string ResourcePath(const std::string& file) { return std::string("data/local/lng/strings/d2rloader/") + kNamespace + "/" + file; }

} // namespace ls
