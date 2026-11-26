// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// D2RCore's stat-name list (MSVC vector<string> layout) grown by one name, on a copy made the way
// D2RCore makes it (0x3b6c50: the names of a real itemstatcost.txt, big arrays 32-aligned with the
// raw pointer before them). The heap counts every block: nothing freed twice, nothing lost.
//   name_list_test <itemstatcost.txt>
#include "../name_list.h"
#include "../table_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

using namespace ltn;

static int g_fail = 0, g_pass = 0;
static void Expect(bool ok, const std::string& what) {
	if (ok) ++g_pass; else { ++g_fail; printf("FAIL  %s\n", what.c_str()); }
}

static std::map<void*, size_t> g_live; // raw block -> size
static size_t g_doubleFree = 0;
static void* Alloc(size_t n) { void* p = malloc(n); g_live[p] = n; return p; }
static void Free(void* p) { if (!g_live.erase(p)) ++g_doubleFree; free(p); }
static const Heap kHeap { Alloc, Free };

// a list as D2RCore builds it: every string made as basic_string makes one
static MsvcVector Build(const std::vector<std::string>& names) {
	MsvcVector v {};
	if (names.empty()) return v;
	const size_t bytes = names.size() * sizeof(MsvcString);
	v.begin = static_cast<uint8_t*>(Allocate(kHeap, bytes));
	v.end = v.cap = v.begin + bytes;
	for (size_t i = 0; i < names.size(); ++i) {
		Prepared one; std::string why;
		MsvcVector empty {};
		Prepare(kHeap, empty, names[i], one, why);          // one string, made the same way
		memcpy(v.begin + i * sizeof(MsvcString), one.array, sizeof(MsvcString));
		Deallocate(kHeap, one.array, one.bytes);            // keep its text block, drop the 1-string array
	}
	return v;
}
static void Destroy(MsvcVector& v) {
	for (uint8_t* at = v.begin; at < v.end; at += sizeof(MsvcString)) {
		auto* s = reinterpret_cast<MsvcString*>(at);
		if (s->cap >= 16) Deallocate(kHeap, s->text.ptr, s->cap + 1);
	}
	Deallocate(kHeap, v.begin, size_t(v.cap - v.begin));
	v = MsvcVector {};
}

int main(int argc, char** argv) {
	std::vector<std::string> names;
	if (argc >= 2) {
		std::ifstream f(argv[1], std::ios::binary);
		std::stringstream s; s << f.rdbuf();
		for (const auto& r : lt::ReadSheet(s.str()).rows) names.push_back(r[0]);
	}
	if (names.empty()) for (int i = 0; i < 389; ++i) names.push_back("stat_" + std::to_string(i));
	printf("names %zu from %s\n", names.size(), argc >= 2 ? argv[1] : "(made up)");

	for (const std::string& grown : { std::string("lt_made"), std::string("a_grown_stat_with_a_long_name_x") }) {
		g_live.clear();
		MsvcVector v = Build(names);
		const size_t blocksBefore = g_live.size();
		std::string why;
		Expect(Sane(v, why) && Count(v) == names.size(), "the copy is a sane list of " + std::to_string(names.size()));
		Expect(names.size() * 0x20 < 0x1000 || reinterpret_cast<uintptr_t>(v.begin) % 32 == 0, "a big array is 32-aligned, as D2RCore's");
		Prepared p;
		Expect(Prepare(kHeap, v, grown, p, why), "prepared with '" + grown + "' " + why);
		Expect(Count(v) == names.size(), "preparing changes nothing in the list");
		uint8_t* oldArray = nullptr; size_t oldBytes = 0;
		const uint8_t* was = v.begin;
		Swap(v, p, oldArray, oldBytes);
		Expect(oldArray == was && Count(v) == names.size() + 1, "swapped: one name more");
		Release(kHeap, oldArray, oldBytes);
		bool same = true;
		for (size_t i = 0; i < names.size(); ++i) same = same && Text(reinterpret_cast<const MsvcString*>(v.begin)[i]) == names[i];
		Expect(same, "every old name still reads the same");
		const MsvcString& last = reinterpret_cast<const MsvcString*>(v.begin)[names.size()];
		Expect(Text(last) == grown && (grown.size() < 16 ? last.cap == 15 : last.cap >= grown.size()), "the new name reads '" + grown + "'");
		Expect(Sane(v, why), "the list is still sane");
		Expect(g_live.size() == blocksBefore + (grown.size() < 16 ? 0 : 1) && g_doubleFree == 0, "blocks: old array freed once, the old names' blocks kept, the new one counted");
		Destroy(v);
		Expect(g_live.empty() && g_doubleFree == 0, "the list freed the way D2RCore frees it: nothing left, nothing twice");
	}
	for (size_t at : { size_t(0), names.size() / 2, names.size() - 1 }) { // a removed stat's name out of the list
		g_live.clear();
		std::vector<std::string> withLong = names;
		withLong[at] = "a_long_removed_stat_name_xyz";            // its own text block must be freed
		MsvcVector v = Build(withLong);
		const size_t blocksBefore = g_live.size();
		Prepared p; std::string why;
		Expect(PrepareWithout(kHeap, v, at, p, why) && p.count == withLong.size() - 1, "prepared without name " + std::to_string(at));
		uint8_t* oldArray = nullptr; size_t oldBytes = 0;
		Swap(v, p, oldArray, oldBytes);
		Release(kHeap, oldArray, oldBytes);
		FreeRemovedText(kHeap, p);
		bool same = Count(v) == withLong.size() - 1;
		for (size_t i = 0, j = 0; i < withLong.size() && same; ++i) { if (i == at) continue; same = Text(reinterpret_cast<const MsvcString*>(v.begin)[j++]) == withLong[i]; }
		Expect(same, "every other name reads the same, in order");
		Expect(g_live.size() == blocksBefore - 1 && g_doubleFree == 0, "blocks: old array swapped for the new one, the removed name's block freed once");
		Destroy(v);
		Expect(g_live.empty() && g_doubleFree == 0, "nothing left, nothing twice (remove)");
	}
	for (const std::string& now : { std::string("lt_short"), std::string("a_renamed_stat_with_a_long_name") }) { // a renamed stat
		g_live.clear();
		std::vector<std::string> withLong = names;
		const size_t at = names.size() / 3;
		withLong[at] = "an_old_long_name_to_be_freed";
		MsvcVector v = Build(withLong);
		const size_t blocksBefore = g_live.size();
		Replaced r; std::string why;
		Expect(PrepareReplace(kHeap, v, at, now, r, why), "prepared with " + now + " at " + std::to_string(at) + " " + why);
		uint8_t* oldArray = nullptr; size_t oldBytes = 0;
		Swap(v, r.p, oldArray, oldBytes);
		Release(kHeap, oldArray, oldBytes);
		FreeOldText(kHeap, r);
		bool same = Count(v) == withLong.size();
		for (size_t i = 0; i < withLong.size() && same; ++i) same = Text(reinterpret_cast<const MsvcString*>(v.begin)[i]) == (i == at ? now : withLong[i]);
		Expect(same, "renamed: the new name at its place, every other as it was");
		Expect(g_live.size() == blocksBefore - 1 + (now.size() >= 16 ? 1 : 0) && g_doubleFree == 0, "blocks: the old name's block freed, the new one's counted");
		Destroy(v);
		Expect(g_live.empty() && g_doubleFree == 0, "nothing left, nothing twice (rename)");
	}
	{   // a list that is not one: refused, nothing allocated
		g_live.clear();
		MsvcVector bad { reinterpret_cast<uint8_t*>(0x1000), reinterpret_cast<uint8_t*>(0x1010), reinterpret_cast<uint8_t*>(0x1010) };
		Prepared p; std::string why;
		Expect(!Prepare(kHeap, bad, "x", p, why) && g_live.empty(), "a list with half a string is refused: " + why);
		Prepared q;
		MsvcVector empty {};
		Expect(Prepare(kHeap, empty, "first", q, why) && q.count == 1, "an empty list takes a first name");
		Discard(kHeap, q);
		Expect(g_live.empty(), "a prepared list not used is given back whole");
	}
	printf("%s  %d checks passed, %d failed\n", g_fail ? "FAIL" : "ok  ", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
