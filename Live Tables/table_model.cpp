// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "table_model.h"

#include <climits>

#include <algorithm>
#include <cstring>

namespace lt {

// ---------------------------------------------------------------- names -------------
std::string Lower(const std::string& s) {
	std::string r = s;
	for (char& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	return r;
}

static uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t Fmix(uint64_t k) {
	k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k;
}
void Murmur3(const void* data, size_t len, uint32_t seed, uint64_t out[2]) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	const size_t blocks = len / 16;
	uint64_t h1 = seed, h2 = seed;
	const uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
	for (size_t i = 0; i < blocks; ++i) {
		uint64_t k1, k2;
		memcpy(&k1, p + i * 16, 8); memcpy(&k2, p + i * 16 + 8, 8);
		k1 *= c1; k1 = Rotl(k1, 31); k1 *= c2; h1 ^= k1;
		h1 = Rotl(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
		k2 *= c2; k2 = Rotl(k2, 33); k2 *= c1; h2 ^= k2;
		h2 = Rotl(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
	}
	const uint8_t* tail = p + blocks * 16;
	uint64_t k1 = 0, k2 = 0;
	switch (len & 15) {
	case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
	case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
	case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
	case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
	case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
	case 10: k2 ^= uint64_t(tail[9]) << 8;  [[fallthrough]];
	case 9:  k2 ^= uint64_t(tail[8]);
	         k2 *= c2; k2 = Rotl(k2, 33); k2 *= c1; h2 ^= k2; [[fallthrough]];
	case 8:  k1 ^= uint64_t(tail[7]) << 56; [[fallthrough]];
	case 7:  k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
	case 6:  k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
	case 5:  k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
	case 4:  k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
	case 3:  k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
	case 2:  k1 ^= uint64_t(tail[1]) << 8;  [[fallthrough]];
	case 1:  k1 ^= uint64_t(tail[0]);
	         k1 *= c1; k1 = Rotl(k1, 31); k1 *= c2; h1 ^= k1;
	}
	h1 ^= len; h2 ^= len;
	h1 += h2; h2 += h1;
	h1 = Fmix(h1); h2 = Fmix(h2);
	h1 += h2; h2 += h1;
	out[0] = h1; out[1] = h2;
}

size_t NameTable::BucketOf(const std::string& lower) const {
	uint64_t h[2];
	Murmur3(lower.data(), lower.size(), 0, h);
	return size_t(h[0] & (buckets.size() - 1));
}

void NameTable::Rehash(size_t want) { // 0x1f7d70: new bucket array, every node relinked
	std::vector<Node*> nodes;
	for (Node* b : buckets) for (Node* n = b; n; n = n->next) nodes.push_back(n);
	buckets.assign(want, nullptr);
	for (Node* n : nodes) {
		n->next = nullptr;
		Node** at = &buckets[BucketOf(n->key)];
		while (*at) at = &(*at)->next;
		*at = n;
	}
	++rehashes;
}

int32_t NameTable::Insert(const std::string& text, bool keepExisting) {
	const std::string key = Lower(text);
	if (key.empty()) emptyNumber = next;              // 0xa11f16
	Node** at = &buckets[BucketOf(key)];               // 0xa0f6e0
	while (*at) {
		if ((*at)->key == key) {                       // found: the new node is freed again
			if (keepExisting) return (*at)->number;    // 0xa11f3c
			return next++;                             // 0xa11f45
		}
		at = &(*at)->next;
	}
	auto node = std::make_unique<Node>();
	node->key = key;
	node->number = next;
	node->next = nullptr;                              // 0xa0f792, linked at the TAIL (0xa0f79d)
	*at = node.get();
	owned.push_back(std::move(node));
	++count;
	if (float(count) / float(buckets.size()) > maxLoad) Rehash(buckets.size() * 2); // 0xa0f7f7
	return next++;
}

bool NameTable::Remove(const std::string& text) {
	const std::string key = Lower(text);
	for (Node** at = &buckets[BucketOf(key)]; *at; at = &(*at)->next)
		if ((*at)->key == key) { *at = (*at)->next; --count; return true; } // the node itself stays (a reader may stand on it)
	return false;
}

bool NameTable::Rename(const std::string& was, const std::string& now) {
	const int32_t n = Find(was);
	if (n < 0 || Find(now) >= 0 || !Remove(was)) return false;
	const int32_t keep = next;
	next = n;
	Insert(now, true);
	next = keep;
	return true;
}

void NameTable::Renumber(int32_t removed) {
	for (Node* b : buckets) for (Node* n = b; n; n = n->next) if (n->number > removed) --n->number;
	if (emptyNumber > removed) --emptyNumber;
	--next;
}

int32_t NameTable::Find(const std::string& text) const {
	if (text.empty()) return emptyNumber;              // 0xa123e5
	const std::string key = Lower(text);
	for (Node* n = buckets[BucketOf(key)]; n; n = n->next)
		if (n->key == key) return n->number;
	return -1;
}

// ---------------------------------------------------------------- columns -----------
const std::vector<Column> kStatColumns = {
	{"stat", KName, 0, 0x0, StatNames},
	{"send bits", KByte, 0, 0x8, 0}, {"send param bits", KByte, 0, 0x9, 0},
	{"multiply", KDword, 0, 0xc, 0}, {"add", KDword, 0, 0x10, 0}, {"valshift", KByte, 0, 0x14, 0},
	{"minaccr", KDword, 0, 0x28, 0}, {"save bits", KByte, 0, 0x15, 0}, {"save add", KDword, 0, 0x18, 0},
	{"save param bits", KDword, 0, 0x20, 0}, {"1.09-save bits", KByte, 0, 0x16, 0},
	{"1.09-save add", KDword, 0, 0x1c, 0}, {"encode", KByte, 0, 0x2c, 0},
	{"send other", KBit, 0, 0x4, 0}, {"signed", KBit, 1, 0x4, 0}, {"updateanimrate", KBit, 8, 0x4, 0},
	{"fmin", KBit, 9, 0x4, 0}, {"damagerelated", KBit, 2, 0x4, 0}, {"direct", KBit, 3, 0x4, 0},
	{"fcallback", KBit, 10, 0x4, 0}, {"saved", KBit, 11, 0x4, 0}, {"csvsigned", KBit, 12, 0x4, 0},
	{"csvbits", KByte, 0, 0xa, 0}, {"csvparam", KByte, 0, 0xb, 0},
	{"maxstat", KLookup, 0, 0x2e, StatNames},
	{"itemevent1", KLookup, 0, 0x44, Events}, {"itemeventfunc1", KWord, 0, 0x48, 0},
	{"itemevent2", KLookup, 0, 0x46, Events}, {"itemeventfunc2", KWord, 0, 0x4a, 0},
	{"descpriority", KWord, 0, 0x30, 0}, {"descfunc", KByte, 0, 0x32, 0}, {"descval", KByte, 0, 0x33, 0},
	{"descstrpos", KString, 0, 0x34, 0}, {"descstrneg", KString, 0, 0x36, 0}, {"descstr2", KString, 0, 0x38, 0},
	{"dgrp", KWord, 0, 0x3a, 0}, {"dgrpfunc", KByte, 0, 0x3c, 0}, {"dgrpval", KByte, 0, 0x3d, 0},
	{"dgrpstrpos", KString, 0, 0x3e, 0}, {"dgrpstrneg", KString, 0, 0x40, 0}, {"dgrpstr2", KString, 0, 0x42, 0},
	{"keepzero", KByte, 0, 0x4c, 0}, {"op", KByte, 0, 0x50, 0},
	{"op base", KLookup, 0, 0x52, StatNames}, {"op param", KByte, 0, 0x51, 0},
	{"op stat1", KLookup, 0, 0x54, StatNames}, {"op stat2", KLookup, 0, 0x56, StatNames},
	{"op stat3", KLookup, 0, 0x58, StatNames},
	{"stuff", KDword, 0, 0x13c, 0}, {"advdisplay", KByte, 0, 0x140, 0},
};
const std::vector<Column> kPropertyColumns = [] {
	std::vector<Column> c = {{"code", KName, 0, 0x0, PropNames}};
	static const char* names[7][4] = {
		{"set1", "val1", "func1", "stat1"}, {"set2", "val2", "func2", "stat2"}, {"set3", "val3", "func3", "stat3"},
		{"set4", "val4", "func4", "stat4"}, {"set5", "val5", "func5", "stat5"}, {"set6", "val6", "func6", "stat6"},
		{"set7", "val7", "func7", "stat7"}};
	for (uint32_t k = 0; k < 7; ++k) {
		c.push_back({names[k][0], KByte, 0, 0x2 + k, 0});
		c.push_back({names[k][1], KWord, 0, 0xa + 2 * k, 0});
		c.push_back({names[k][2], KByte, 0, 0x18 + k, 0});
		c.push_back({names[k][3], KLookup, 0, 0x20 + 2 * k, StatNames});
	}
	c.push_back({"uiRangeType", KWord, 0, 0x2e, 0});
	return c;
}();
const std::vector<Column>& ColumnsOf(TableId t) { return t == TableId::ItemStatCost ? kStatColumns : kPropertyColumns; }
size_t RowSizeOf(TableId t) { return t == TableId::ItemStatCost ? kStatRow : kPropRow; }
int NamesOf(TableId t) { return t == TableId::ItemStatCost ? StatNames : PropNames; }
const char* TableFileName(TableId t) { return t == TableId::ItemStatCost ? "itemstatcost" : "properties"; }

// ---------------------------------------------------------------- text --------------
static std::vector<std::string> Split(const std::string& line, char by) {
	std::vector<std::string> cells;
	size_t c = 0;
	for (;;) {
		const size_t t = line.find(by, c);
		cells.push_back(line.substr(c, t == std::string::npos ? std::string::npos : t - c));
		if (t == std::string::npos) break;
		c = t + 1;
	}
	return cells;
}

Sheet ReadSheet(const std::string& text) {
	Sheet s;
	size_t at = 0;
	bool first = true;
	while (at < text.size()) {
		size_t end = text.find('\n', at);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		std::vector<std::string> cells = Split(line, '\t');
		if (first) { s.header = cells; first = false; continue; }
		if (Lower(cells[0]) == "expansion") continue;   // tokenizer 0xa1528c: the row count goes down
		s.rows.push_back(std::move(cells));
	}
	return s;
}

static int HeaderIndex(const std::vector<std::string>& header, const std::string& name) {
	const std::string l = Lower(name);
	for (size_t i = 0; i < header.size(); ++i) if (Lower(header[i]) == l) return int(i);
	return -1;
}

bool RowCells(const std::vector<std::string>& header, const std::string& rowText, std::vector<std::string>& cells, std::string& why) {
	std::string text = rowText;
	while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
	if (text.find('\n') != std::string::npos) { why = "the text is more than one row"; return false; }
	const std::vector<std::string> parts = Split(text, '\t');
	bool pairs = !text.empty();
	for (const std::string& p : parts) {
		if (p.empty()) continue;
		const size_t eq = p.find('=');
		if (eq == std::string::npos || HeaderIndex(header, p.substr(0, eq)) < 0) { pairs = false; break; }
	}
	cells.assign(header.size(), std::string());
	if (pairs) {
		for (const std::string& p : parts) {
			if (p.empty()) continue;
			const size_t eq = p.find('=');
			cells[size_t(HeaderIndex(header, p.substr(0, eq)))] = p.substr(eq + 1);
		}
		return true;
	}
	if (parts.size() > header.size()) { why = "the row has more cells than the table has columns"; return false; }
	for (size_t i = 0; i < parts.size(); ++i) cells[i] = parts[i];
	return true;
}

std::string PairsOf(const std::vector<std::string>& header, const std::vector<std::string>& cells) {
	std::string r;
	for (size_t i = 0; i < header.size() && i < cells.size(); ++i) {
		if (cells[i].empty()) continue;
		if (!r.empty()) r += '\t';
		r += header[i] + "=" + cells[i];
	}
	return r;
}

std::string NameIn(const std::vector<std::string>& header, const std::vector<std::string>& cells, TableId t) {
	for (const Column& c : ColumnsOf(t))
		if (c.kind == KName) {
			const int i = HeaderIndex(header, c.name);
			return i >= 0 && size_t(i) < cells.size() ? cells[size_t(i)] : std::string();
		}
	return std::string();
}

// ---------------------------------------------------------------- compile -----------
static int32_t Number(const std::string& s) { // 0xa1319f: '-' then 10*n + (c - '0') for EVERY letter
	size_t i = 0;
	const bool neg = !s.empty() && s[0] == '-';
	if (neg) i = 1;
	uint32_t v = 0;
	for (; i < s.size(); ++i) v = v * 10u + uint32_t(int32_t(int8_t(s[i])) - 48);
	return neg ? -int32_t(v) : int32_t(v);
}

static void Put(uint8_t* row, const Column& c, uint32_t v) {
	switch (c.kind) {
	case KDword: memcpy(row + c.offset, &v, 4); break;
	case KWord: case KLookup: case KString: case KName: { uint16_t w = uint16_t(v); memcpy(row + c.offset, &w, 2); break; }
	case KByte: row[c.offset] = uint8_t(v); break;
	case KBit: { uint32_t d; memcpy(&d, row + c.offset, 4); if (v) d |= 1u << c.n; else d &= ~(1u << c.n); memcpy(row + c.offset, &d, 4); break; }
	}
}

// 0xa101f0: file column -> descriptor (-1 = not one of ours); the descriptors no file column names
struct Map { std::vector<int> file; std::vector<int> missing; };
static Map MapColumns(const std::vector<std::string>& header, const std::vector<Column>& cols) {
	Map m;
	std::vector<bool> seen(cols.size(), false);
	for (const std::string& h : header) {
		int k = -1;
		for (size_t i = 0; i < cols.size(); ++i)
			if (!seen[i] && Lower(h) == Lower(cols[i].name)) { k = int(i); seen[i] = true; break; }
		m.file.push_back(k);
	}
	for (size_t i = 0; i < cols.size(); ++i) if (!seen[i]) m.missing.push_back(int(i));
	return m;
}

static void FirstPass(const Map& m, const std::vector<std::string>& cells, const std::vector<Column>& cols, uint8_t* row, const Resolver& r) {
	for (size_t f = 0; f < m.file.size(); ++f) {
		if (m.file[f] < 0 || cols[m.file[f]].kind != KName) continue;
		const Column& c = cols[m.file[f]];
		std::string text = f < cells.size() ? cells[f] : std::string();
		if (text.size() > 0x2f) text.resize(0x2f);  // 0xa12b20 (longer = assert) / the copy stops at 0x2f
		Put(row, c, uint32_t(r.insert(c.table, text)));  // 0xa12ba3, keepExisting = 1
	}
}

static void SecondPass(const Map& m, const std::vector<std::string>& cells, const std::vector<Column>& cols, uint8_t* row, const Resolver& r) {
	for (size_t f = 0; f < m.file.size(); ++f) {
		if (m.file[f] < 0) continue;
		const Column& c = cols[m.file[f]];
		const std::string text = f < cells.size() ? cells[f] : std::string();
		switch (c.kind) {
		case KName: break;                                                 // 0xa12e8e: done in pass 1
		case KLookup: Put(row, c, uint32_t(r.find(c.table, text))); break;  // 0xa1314a, -1 -> 0xffff
		case KString: Put(row, c, r.stringId(text)); break;                 // 0xa132f5
		default: Put(row, c, uint32_t(Number(text))); break;
		}
	}
	for (int k : m.missing) {                                              // 0xa13080 loop
		const Column& c = cols[size_t(k)];
		switch (c.kind) {
		case KLookup: Put(row, c, 0xffff); break;                          // 0xa13402
		case KBit: break;                                                  // 0xa13474: untouched
		default: Put(row, c, 0); break;
		}
	}
}

std::vector<uint8_t> Compile(const Sheet& sheet, TableId t, const Resolver& r) {
	const size_t rowSize = RowSizeOf(t);
	const std::vector<Column>& cols = ColumnsOf(t);
	std::vector<uint8_t> rows(sheet.rows.size() * rowSize, 0);
	const Map m = MapColumns(sheet.header, cols);
	for (size_t i = 0; i < sheet.rows.size(); ++i) FirstPass(m, sheet.rows[i], cols, &rows[i * rowSize], r);
	for (size_t i = 0; i < sheet.rows.size(); ++i) SecondPass(m, sheet.rows[i], cols, &rows[i * rowSize], r);
	return rows;
}

void CompileRow(const std::vector<std::string>& header, const std::vector<std::string>& cells, TableId t, uint8_t* row, const Resolver& r) {
	const Map m = MapColumns(header, ColumnsOf(t));
	memset(row, 0, RowSizeOf(t));
	FirstPass(m, cells, ColumnsOf(t), row, r);
	SecondPass(m, cells, ColumnsOf(t), row, r);
}

// ---------------------------------------------------------------- derived -----------
static uint16_t W(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void SetW(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static const uint8_t kOpFlag[4] = {0x10, 0x20, 0x40, 0x80}; // dwords at 0x1d996e0..0x1d996ec
static uint8_t* RowAt(uint8_t* rows, size_t i) { return rows + i * kStatRow; }

void LinkStat(uint8_t* rows, size_t count, size_t i, bool ordered) { // one turn of 0x31ee83..0x31f09d
	uint8_t* row = RowAt(rows, i);
	if (uint8_t(row[0x50] - 1) > 0xc) { row[0x50] = 0; return; }     // 0x31f08c: op 0 or > 13
	const uint16_t base = W(row + 0x52);
	if (base < count) {
		uint8_t* b = RowAt(rows, base);
		b[0x4d] = 1;
		size_t k = 0;
		while (k < 0x40 && W(b + 0x5a + 2 * k) < count && (!ordered || W(b + 0x5a + 2 * k) < i)) ++k;
		if (k < 0x40) {
			if (ordered) memmove(b + 0x5a + 2 * (k + 1), b + 0x5a + 2 * k, 2 * (0x3f - k)); // the last one drops out, as at load
			SetW(b + 0x5a + 2 * k, uint16_t(i));
		}
		if (row[0x50] == 4 || row[0x50] == 5) row[0x4f] = 1;
	}
	for (size_t s = 0; s < 3; ++s) {
		row[0x4d] = 1;
		const uint16_t target = W(row + 0x54 + 2 * s);
		if (target >= count) return;                                       // 0x31ef65: stops at the first unknown
		uint8_t* tr = RowAt(rows, target);
		size_t e = 0;
		while (e < 0x10 && tr[0xda + 6 * e + 4] != 0 && (!ordered || W(tr + 0xda + 6 * e + 2) <= i)) ++e;
		if (e >= 0x10 || (!ordered && tr[0xda + 6 * e + 4] != 0)) continue; // 16 taken: nothing
		// ordered and full: the highest source drops out, as it would not have found room at load
		// (its flag bits at +4 are not redone: only when a stat has 16 op sources)
		if (ordered) memmove(tr + 0xda + 6 * (e + 1), tr + 0xda + 6 * e, 6 * (0xf - e));
		uint8_t* en = tr + 0xda + 6 * e;
		SetW(en + 2, uint16_t(i));
		en[4] = row[0x50];
		SetW(en + 0, base);
		en[5] = row[0x51];
		tr[0x4e] = 1;
		const auto hit = [&](size_t n) { return i == n || target == n; };
		if (hit(7) || hit(9) || hit(0xb)) {
			row[4] |= hit(7) ? kOpFlag[1] : hit(9) ? kOpFlag[2] : kOpFlag[3];
			row[4] |= kOpFlag[0];
		}
	}
}

void UnlinkStat(uint8_t* rows, size_t count, size_t i) {
	for (size_t b = 0; b < count; ++b) {
		uint8_t* r = RowAt(rows, b);
		for (size_t k = 0; k < 0x40; ++k)
			if (W(r + 0x5a + 2 * k) == i) {
				memmove(r + 0x5a + 2 * k, r + 0x5a + 2 * (k + 1), 2 * (0x3f - k));
				SetW(r + 0x5a + 2 * 0x3f, 0xffff);
				--k;
			}
		for (size_t e = 0; e < 0x10; ++e)
			if (r[0xda + 6 * e + 4] != 0 && W(r + 0xda + 6 * e + 2) == i) {
				memmove(r + 0xda + 6 * e, r + 0xda + 6 * (e + 1), 6 * (0xf - e));
				memset(r + 0xda + 6 * 0xf, 0, 6);
				--e;
			}
	}
	RowAt(rows, i)[4] &= 0x0f; // bits 4-7 come only from the linker (no column sets them)
}

void LinkStats(uint8_t* rows, size_t count) {
	for (size_t i = 0; i < count; ++i) memset(RowAt(rows, i) + 0x5a, 0xff, 0x80); // 0x31edd1
	for (size_t i = 0; i < count; ++i) LinkStat(rows, count, i, false);
}

std::vector<uint16_t> DescOrder(const uint8_t* rows, size_t count) {
	std::vector<uint16_t> order;
	for (size_t i = 0; i < count; ++i) if (rows[i * kStatRow + 0x32] != 0) order.push_back(uint16_t(i));
	std::stable_sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
		return int16_t(W(rows + a * kStatRow + 0x30)) < int16_t(W(rows + b * kStatRow + 0x30));
	});
	return order;
}

// ---------------------------------------------------------------- disable / remove ---
OffCells DisableStat(uint8_t* rows, size_t count, size_t i) {
	uint8_t* r = RowAt(rows, i);
	OffCells was { r[0x50], r[0x32], W(r + 0x3a), W(r + 0x44), W(r + 0x46) };
	UnlinkStat(rows, count, i);
	r[0x50] = 0; r[0x32] = 0; SetW(r + 0x3a, 0); SetW(r + 0x44, 0xffff); SetW(r + 0x46, 0xffff);
	// UnlinkStat took i out of every list; the lists of OTHER rows naming i (i as their base / target) are untouched
	return was;
}

void EnableStat(uint8_t* rows, size_t count, size_t i, const OffCells& was) {
	uint8_t* r = RowAt(rows, i);
	r[0x50] = was.op; r[0x32] = was.descfunc; SetW(r + 0x3a, was.dgrp); SetW(r + 0x44, was.event1); SetW(r + 0x46, was.event2);
	LinkStat(rows, count, i, true);
}

bool IsInert(const uint8_t* r) {
	return r[0x50] == 0 && r[0x32] == 0 && W(r + 0x3a) == 0 && W(r + 0x44) == 0xffff && W(r + 0x46) == 0xffff;
}

std::vector<std::string> StatNamedBy(const uint8_t* stats, size_t count, size_t n, const uint8_t* props, size_t propCount) {
	std::vector<std::string> out;
	static const std::pair<uint32_t, const char*> kStatWords[] = { {0x52, "op base"}, {0x54, "op stat1"}, {0x56, "op stat2"}, {0x58, "op stat3"}, {0x2e, "maxstat"} };
	for (size_t i = 0; i < count; ++i) {
		if (i == n) continue;
		for (const auto& [off, what] : kStatWords) if (W(stats + i * kStatRow + off) == n) out.push_back("stat " + std::to_string(i) + " " + what);
	}
	for (size_t p = 0; p < propCount; ++p)
		for (uint32_t k = 0; k < 7; ++k) if (W(props + p * kPropRow + 0x20 + 2 * k) == n) out.push_back("property " + std::to_string(p) + " stat" + std::to_string(k + 1));
	return out;
}

void RemoveStatRows(const uint8_t* rows, size_t count, size_t n, uint8_t* out) {
	size_t o = 0;
	for (size_t i = 0; i < count; ++i) {
		if (i == n) continue;
		uint8_t* r = out + o * kStatRow;
		memcpy(r, rows + i * kStatRow, kStatRow);
		for (uint32_t off : { 0x0u, 0x2eu, 0x52u, 0x54u, 0x56u, 0x58u }) {
			const uint16_t v = W(r + off);
			if (v != 0xffff && v > n) SetW(r + off, uint16_t(v - 1));
		}
		// the links come from the cells again, as at load: lists and the linker's own marks cleared
		memset(r + 0xda, 0, 0x60);
		r[0x4d] = r[0x4e] = r[0x4f] = 0;
		r[4] &= 0x0f;
		++o;
	}
	LinkStats(out, count - 1);
}

size_t RenumberPropStats(uint8_t* props, size_t propCount, size_t n) {
	size_t changed = 0;
	for (size_t p = 0; p < propCount; ++p) {
		bool any = false;
		for (uint32_t k = 0; k < 7; ++k) {
			uint8_t* at = props + p * kPropRow + 0x20 + 2 * k;
			const uint16_t v = W(at);
			if (v != 0xffff && v > n) { SetW(at, uint16_t(v - 1)); any = true; }
		}
		changed += any;
	}
	return changed;
}

// ---------------------------------------------------------------- growing -----------
Check CheckRow(TableId t, const Sheet& sheet, size_t count, int32_t nameNext, size_t limit, const std::string& rowText, const Resolver& r) {
	Check c;
	if (sheet.header.empty()) { c.why = "the text the game read for this table was not seen (the read hook is off)"; return c; }
	if (sheet.rows.size() != count) {
		c.why = "the table has " + std::to_string(count) + " rows but its text " + std::to_string(sheet.rows.size()) + ": something else changed the table";
		return c;
	}
	if (!RowCells(sheet.header, rowText, c.cells, c.why)) return c;
	c.name = NameIn(sheet.header, c.cells, t);
	if (c.name.empty()) { c.why = std::string("the row has no ") + (t == TableId::ItemStatCost ? "stat name" : "code"); return c; }
	if (c.name.size() > 0x2f) { c.why = "the name is longer than 47 letters (the game cuts it)"; return c; }
	if (r.find(NamesOf(t), c.name) >= 0) { c.why = "the name " + c.name + " is taken"; return c; }
	if (nameNext < 0 || size_t(nameNext) != count) {
		c.why = "the name table would give number " + std::to_string(nameNext) + ", not the row number " + std::to_string(count);
		return c;
	}
	if (count + 1 > limit) {
		c.why = t == TableId::ItemStatCost ? (limit == kMaxStatsRetail ? "stat numbers end at 510 with the game's own save format" : "the game takes at most 32768 stats")
		                                   : "property numbers end at 32766";
		return c;
	}
	c.number = count;
	c.ok = true;
	return c;
}

std::vector<std::pair<size_t, const Column*>> RowsNaming(TableId t, const Sheet& sheet, int names, const std::string& name) {
	std::vector<std::pair<size_t, const Column*>> out;
	const Map m = MapColumns(sheet.header, ColumnsOf(t));
	const std::string l = Lower(name);
	for (size_t row = 0; row < sheet.rows.size(); ++row)
		for (size_t f = 0; f < m.file.size() && f < sheet.rows[row].size(); ++f) {
			if (m.file[f] < 0) continue;
			const Column& c = ColumnsOf(t)[size_t(m.file[f])];
			if (c.kind == KLookup && c.table == names && Lower(sheet.rows[row][f]) == l) out.push_back({row, &c});
		}
	return out;
}

std::vector<size_t> RelinkCells(TableId t, const Sheet& sheet, uint8_t* rows, size_t count, int names, const std::string& name, uint16_t number) {
	std::vector<size_t> changed;
	for (const auto& [row, col] : RowsNaming(t, sheet, names, name)) {
		if (row >= count) continue;
		uint8_t* at = rows + row * RowSizeOf(t) + col->offset;
		if (W(at) != 0xffff) continue; // another plugin set it: left alone
		SetW(at, number);
		if (changed.empty() || changed.back() != row) changed.push_back(row);
	}
	return changed;
}

Grown GrowInto(TableId t, const Sheet& sheet, const Check& c, uint8_t* next, const Resolver& r) {
	Grown g;
	g.number = c.number;
	const size_t count = c.number + 1;
	uint8_t* row = next + c.number * RowSizeOf(t);
	CompileRow(sheet.header, c.cells, t, row, r);                       // pass 1 puts the name in
	g.relinked = RelinkCells(t, sheet, next, c.number, NamesOf(t), c.name, uint16_t(c.number));
	if (t == TableId::ItemStatCost) {
		memset(row + 0x5a, 0xff, 0x80);
		for (size_t j : g.relinked) { UnlinkStat(next, count, j); LinkStat(next, count, j, true); }
		LinkStat(next, count, c.number, true);
	}
	return g;
}

// ---------------------------------------------------------------- names in texts ---
static bool NameEdge(char c, bool before) {
	return c == '\t' || c == '\n' || c == '\r' || c == '\'' || c == '"' || c == ',' || (before ? c == '(' : (c == '.' || c == ')'));
}

std::vector<size_t> NamedRows(const std::string& text, const std::string& name) {
	std::vector<size_t> rows;
	const std::string key = Lower(name);
	if (key.empty()) return rows;
	const std::string lower = Lower(text);
	size_t line = 0, counted = 0; // lines up to `counted`
	for (size_t at = lower.find(key); at != std::string::npos; at = lower.find(key, at + 1)) {
		const char before = at == 0 ? '\n' : lower[at - 1], after = at + key.size() < lower.size() ? lower[at + key.size()] : '\n';
		if (!NameEdge(before, true) || !NameEdge(after, false)) continue;
		line += size_t(std::count(lower.begin() + long(counted), lower.begin() + long(at), '\n'));
		counted = at;
		if (line != 0 && (rows.empty() || rows.back() != line)) rows.push_back(line);
	}
	return rows;
}

// ---------------------------------------------------------------- persistence -------
std::string KeptLine(const KeptRow& k) {
	std::string r = std::string(TableFileName(k.table)) + "\tv=" + k.versions + "\tn=" + std::to_string(k.number);
	if (k.numberBase != SIZE_MAX) r += "\tnb=" + std::to_string(k.numberBase);
	for (const auto& [col, val] : k.cells) r += "\t" + col + "=" + val;
	return r;
}

size_t KeptNumberFor(const KeptRow& k, int which) {
	const bool in3 = k.versions.find('3') != std::string::npos;
	if (which == 0) return in3 ? k.number : SIZE_MAX;
	if (k.numberBase != SIZE_MAX) return k.numberBase;
	return in3 ? SIZE_MAX : k.number;
}

std::vector<Moved> MovedRows(const std::vector<Placed>& placed, TableId t, int which) {
	std::vector<Moved> out;
	for (const Placed& p : placed) {
		const size_t was = KeptNumberFor(*p.row, which);
		if (was != SIZE_MAX && was != p.number) out.push_back({t, which, p.row->name, was, p.number});
	}
	return out;
}

std::vector<KeptRow> ReadKept(const std::string& file, std::vector<std::string>* problems) {
	std::vector<KeptRow> out;
	size_t at = 0, lineNo = 0;
	while (at < file.size()) {
		size_t end = file.find('\n', at);
		if (end == std::string::npos) end = file.size();
		std::string line = file.substr(at, end - at);
		at = end + 1;
		++lineNo;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		const std::vector<std::string> parts = Split(line, '\t');
		KeptRow k;
		if (Lower(parts[0]) == "itemstatcost") k.table = TableId::ItemStatCost;
		else if (Lower(parts[0]) == "properties") k.table = TableId::Properties;
		else { if (problems) problems->push_back("line " + std::to_string(lineNo) + ": unknown table " + parts[0]); continue; }
		for (size_t i = 1; i < parts.size(); ++i) {
			const size_t eq = parts[i].find('=');
			if (eq == std::string::npos) continue;
			const std::string key = parts[i].substr(0, eq), val = parts[i].substr(eq + 1);
			if (i == 1 && key == "v") { k.versions = val; continue; }
			if (i == 2 && key == "n") { k.number = size_t(strtoul(val.c_str(), nullptr, 10)); continue; }
			if (i == 3 && key == "nb") { k.numberBase = size_t(strtoul(val.c_str(), nullptr, 10)); continue; }
			k.cells.push_back({key, val});
			for (const Column& c : ColumnsOf(k.table)) if (c.kind == KName && Lower(key) == c.name) k.name = val;
		}
		if (k.name.empty() || k.versions.empty()) { if (problems) problems->push_back("line " + std::to_string(lineNo) + ": no name or no data version"); continue; }
		out.push_back(std::move(k));
	}
	return out;
}

std::string AppendKept(const std::string& text, TableId t, const std::vector<const KeptRow*>& rows, std::vector<std::string>& added, std::vector<std::string>& notes,
                       std::vector<Placed>* placed) {
	const Sheet s = ReadSheet(text);
	if (s.header.empty()) return text;
	std::vector<std::string> have;
	for (const auto& r : s.rows) have.push_back(Lower(NameIn(s.header, r, t)));
	const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	std::string out = text;
	if (!out.empty() && out.back() != '\n') out += eol;
	for (const KeptRow* k : rows) {
		if (const auto it = std::find(have.begin(), have.end(), Lower(k->name)); it != have.end()) {
			const size_t at = size_t(it - have.begin());
			notes.push_back(k->name + ": already in the mod's table, at number " + std::to_string(at) + "; that row is used");
			if (placed) placed->push_back({k, at, true});
			continue;
		}
		std::vector<std::string> cells(s.header.size());
		for (const auto& [col, val] : k->cells) {
			const int i = HeaderIndex(s.header, col);
			if (i < 0) notes.push_back(k->name + ": column " + col + " is not in this table's header, left out");
			else cells[size_t(i)] = val;
		}
		std::string line;
		for (size_t i = 0; i < cells.size(); ++i) line += (i ? "\t" : "") + cells[i];
		out += line + eol;
		if (placed) placed->push_back({k, have.size(), false});
		have.push_back(Lower(k->name));
		added.push_back(k->name);
	}
	return out;
}

// ---------------------------------------------------------------- journal -----------
std::string JournalLine(const std::string& name, size_t n, const std::string& backup) {
	return "# Live Tables: a stat remove was in progress (do not edit)\nitemstatcost\t" + name + "\t" + std::to_string(n) + "\t" + backup + "\n";
}

std::string CellsJournalLine(const std::string& name, size_t n, const std::string& backup, const std::vector<std::pair<std::string, std::string>>& cells) {
	std::string r = "# Live Tables: a cell change with a save resize was in progress (do not edit)\nsetcells\t" + name + "\t" + std::to_string(n) + "\t" + backup;
	for (const auto& [c, v] : cells) r += "\t" + c + "=" + v;
	return r + "\n";
}

std::string KeptCell(const KeptRow& k, const std::string& column) {
	for (const auto& [c, v] : k.cells) if (Lower(c) == Lower(column)) return v;
	return {};
}

void SetKeptCells(KeptRow& k, const std::vector<std::pair<std::string, std::string>>& cells) {
	for (const auto& [c, v] : cells) {
		bool done = false;
		for (auto it = k.cells.begin(); it != k.cells.end(); ++it)
			if (Lower(it->first) == Lower(c)) { if (v.empty()) k.cells.erase(it); else it->second = v; done = true; break; }
		if (!done && !v.empty()) k.cells.push_back({c, v});
	}
}

bool PairVersions(const std::vector<int>& targets, const std::vector<size_t>& numbers, std::vector<VersionRow>& out) {
	out.clear();
	if (targets.size() != numbers.size()) return false;
	for (size_t k = 0; k < targets.size(); ++k) out.push_back({ targets[k], numbers[k] });
	return true;
}

void ReportNumber(int32_t (&numbers)[4], int version, size_t number, bool oneObject12) {
	if (version < 1 || version > 3) return;
	const int32_t n = number > INT32_MAX ? -1 : static_cast<int32_t>(number);
	numbers[version] = n;
	if (version == 1 && oneObject12) numbers[2] = n;
}

size_t SameNumber(const std::vector<VersionRow>& rows) {
	if (rows.empty()) return SIZE_MAX;
	for (const VersionRow& r : rows) if (r.number != rows[0].number) return SIZE_MAX;
	return rows[0].number;
}

bool ReconcileSaveShape(std::vector<KeptRow>& kept, const std::string& statEdits, std::vector<std::string>& notes) {
	static const struct { size_t field; const char* column; } kShape[] = { {1, "Signed"}, {2, "Send Bits"}, {3, "Save Bits"}, {4, "Save Add"} };
	bool changed = false;
	for (size_t at = 0; at < statEdits.size();) {
		size_t end = statEdits.find('\n', at); if (end == std::string::npos) end = statEdits.size();
		std::string line = statEdits.substr(at, end - at); at = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::vector<std::string> f;
		for (size_t p = 0;;) { const size_t t = line.find('\t', p); f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p)); if (t == std::string::npos) break; p = t + 1; }
		if (f.size() < 18 || f[17].empty() || f[17] == "-") continue;
		for (KeptRow& k : kept) {
			if (k.table != TableId::ItemStatCost || Lower(k.name) != Lower(f[17])) continue;
			for (const auto& s : kShape) {
				const std::string& theirs = f[s.field];
				if (theirs.empty() || theirs == "-") continue;
				const std::string ours = KeptCell(k, s.column);
				if (ours == theirs) continue;
				SetKeptCells(k, {{s.column, theirs}});
				notes.push_back(k.name + " " + s.column + ": " + (ours.empty() ? std::string("(none)") : ours) + " -> " + theirs + " (from Live Properties' stat-edits.txt, line of stat " + f[0] + ")");
				changed = true;
			}
		}
	}
	return changed;
}

JournalStep DecideJournal(const std::string& journal, const std::vector<KeptRow>& kept) {
	JournalStep s;
	std::string line;
	for (size_t at = 0; at < journal.size();) {
		size_t end = journal.find('\n', at); if (end == std::string::npos) end = journal.size();
		std::string l = journal.substr(at, end - at); at = end + 1;
		if (!l.empty() && l.back() == '\r') l.pop_back();
		if (!l.empty() && l[0] != '#') line = l;
	}
	if (line.empty()) return s;
	const std::vector<std::string> f = Split(line, '\t');
	s.name = f.size() > 1 ? f[1] : "";
	s.number = f.size() > 2 ? size_t(strtoul(f[2].c_str(), nullptr, 10)) : 0;
	s.backup = f.size() > 3 ? f[3] : "";
	if (f[0] == "setcells") { // ended when the kept row holds every cell the change wanted
		const KeptRow* row = nullptr;
		for (const KeptRow& k : kept) if (k.table == TableId::ItemStatCost && Lower(k.name) == Lower(s.name)) row = &k;
		bool holds = row != nullptr;
		for (size_t i = 4; i < f.size() && holds; ++i) {
			const size_t eq = f[i].find('=');
			if (eq != std::string::npos) holds = KeptCell(*row, f[i].substr(0, eq)) == f[i].substr(eq + 1);
		}
		s.what = holds ? JournalStep::Ended : s.backup.empty() ? JournalStep::StoppedInFix : JournalStep::Restore;
		return s;
	}
	bool still = false;
	for (const KeptRow& k : kept) still = still || (k.table == TableId::ItemStatCost && Lower(k.name) == Lower(s.name));
	s.what = !still ? JournalStep::Ended : s.backup.empty() ? JournalStep::StoppedInFix : JournalStep::Restore;
	return s;
}

// ---------------------------------------------------------------- rename ------------
static bool Names(const Column& c, TableId t) { // a cell that names a row of table t
	return (c.kind == KName && false) || (c.kind == KLookup && c.table == NamesOf(t));
}

size_t RenameInKept(std::vector<KeptRow>& kept, TableId t, const std::string& was, const std::string& now) {
	size_t changed = 0;
	const std::string w = Lower(was);
	for (KeptRow& k : kept)
		for (auto& [col, val] : k.cells) {
			if (Lower(val) != w) continue;
			for (const Column& c : ColumnsOf(k.table)) {
				if (Lower(col) != c.name) continue;
				const bool own = k.table == t && c.kind == KName;
				if (own || Names(c, t)) { val = now; ++changed; if (own) k.name = now; }
			}
		}
	return changed;
}

size_t RenameInSheet(Sheet& sheet, TableId sheetTable, TableId t, const std::string& was, const std::string& now) {
	size_t changed = 0;
	const std::string w = Lower(was);
	const Map m = MapColumns(sheet.header, ColumnsOf(sheetTable));
	for (auto& row : sheet.rows)
		for (size_t f = 0; f < m.file.size() && f < row.size(); ++f) {
			if (m.file[f] < 0 || Lower(row[f]) != w) continue;
			const Column& c = ColumnsOf(sheetTable)[size_t(m.file[f])];
			if ((sheetTable == t && c.kind == KName) || Names(c, t)) { row[f] = now; ++changed; }
		}
	return changed;
}

// ---------------------------------------------------------------- the model ---------
Resolver Model::resolver() {
	Resolver r;
	r.find = [this](int n, const std::string& s) { return names[n].Find(s); };
	r.insert = [this](int n, const std::string& s) { return names[n].Insert(s, true); };
	auto ids = strings;
	r.stringId = [ids](const std::string& k) -> uint16_t { // stand-in for 0x308be0: one number per key, whatever the order
		if (k.empty()) return 0;
		uint64_t h[2];
		Murmur3(k.data(), k.size(), 0, h);
		return uint16_t(1 + h[0] % 60000);
	};
	return r;
}

void Load(Model& m, const std::string& statText, const std::string& propText) {
	const Resolver r = m.resolver();
	m.stats.sheet = ReadSheet(statText);
	m.stats.bytes = Compile(m.stats.sheet, TableId::ItemStatCost, r);
	m.stats.rows = m.stats.sheet.rows.size();
	LinkStats(m.stats.bytes.data(), m.stats.rows);
	m.descOrder = DescOrder(m.stats.bytes.data(), m.stats.rows);
	m.props.sheet = ReadSheet(propText);
	m.props.bytes = Compile(m.props.sheet, TableId::Properties, r);
	m.props.rows = m.props.sheet.rows.size();
}

GrowResult Grow(Model& m, TableId t, const std::string& rowText) {
	GrowResult g;
	ModelTable& tab = m.Of(t);
	const Resolver r = m.resolver();
	const size_t limit = t == TableId::ItemStatCost ? (m.retailSaves ? kMaxStatsRetail : kMaxStatsGame) : kMaxProperties;
	const Check c = CheckRow(t, tab.sheet, tab.rows, m.names[NamesOf(t)].next, limit, rowText, r);
	if (!c.ok) { g.why = c.why; return g; }
	g.rehash = m.names[NamesOf(t)].InsertRehashes();
	const size_t rs = RowSizeOf(t);
	std::vector<uint8_t> next(tab.bytes.begin(), tab.bytes.begin() + long(tab.rows * rs)); // the old rows as they are
	next.resize((tab.rows + 1) * rs, 0);
	const Grown gr = GrowInto(t, tab.sheet, c, next.data(), r);
	tab.retired.push_back(std::move(tab.bytes));
	tab.bytes = std::move(next);
	tab.rows += 1;
	tab.sheet.rows.push_back(c.cells);
	g.relinked = gr.relinked;
	if (t == TableId::ItemStatCost) {
		// a new stat may be named by older property rows too
		g.relinkedProps = RelinkCells(TableId::Properties, m.props.sheet, m.props.bytes.data(), m.props.rows, StatNames, c.name, uint16_t(c.number));
		m.descOrder = DescOrder(tab.bytes.data(), tab.rows);
	}
	g.ok = true;
	g.number = c.number;
	return g;
}

GrowResult RemoveStat(Model& m, size_t n) {
	GrowResult g;
	ModelTable& st = m.stats;
	if (n >= st.rows) { g.why = "no such row"; return g; }
	const std::vector<std::string> named = StatNamedBy(st.bytes.data(), st.rows, n, m.props.bytes.data(), m.props.rows);
	if (!named.empty()) { g.why = "still named by " + named[0]; return g; }
	std::vector<uint8_t> next((st.rows - 1) * kStatRow);
	RemoveStatRows(st.bytes.data(), st.rows, n, next.data());
	const std::string name = NameIn(st.sheet.header, st.sheet.rows[n], TableId::ItemStatCost);
	m.names[StatNames].Remove(name);
	m.names[StatNames].Renumber(int32_t(n));
	st.retired.push_back(std::move(st.bytes));
	st.bytes = std::move(next);
	st.rows -= 1;
	st.sheet.rows.erase(st.sheet.rows.begin() + long(n));
	RenumberPropStats(m.props.bytes.data(), m.props.rows, n);
	m.descOrder = DescOrder(st.bytes.data(), st.rows);
	g.ok = true;
	g.number = n;
	return g;
}

GrowResult RenameRow(Model& m, TableId t, size_t n, const std::string& now) {
	GrowResult g;
	ModelTable& tab = m.Of(t);
	if (n >= tab.rows) { g.why = "no such row"; return g; }
	const std::string was = NameIn(tab.sheet.header, tab.sheet.rows[n], t);
	if (!m.names[NamesOf(t)].Rename(was, now)) { g.why = "the new name is taken"; return g; }
	RenameInSheet(m.stats.sheet, TableId::ItemStatCost, t, was, now);
	RenameInSheet(m.props.sheet, TableId::Properties, t, was, now);
	g.ok = true;
	g.number = n;
	return g;
}

} // namespace lt
