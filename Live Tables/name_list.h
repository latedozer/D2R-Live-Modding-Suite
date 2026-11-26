// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
//
// D2RCore's own stat-name lists, one per data version (D2RCore 0x7de2e0 + version * 0x18), read by
// the character environment digest (0x3b75e0 -> 0x3bc9a0) and filled from the itemstatcost text when
// the game reads it (0x3b6c50). Each is a std::vector<std::string> as D2RCore's compiler (MSVC) lays
// it out; D2RCore replaces a whole list under its exclusive SRW lock 0x7de340 (0x3b745f..0x3b74bc).
// Live Tables appends a grown stat's name the same way: a new array built to the side, swapped in
// under that lock, the old array freed after. Portable: the heap is given (D2RCore's own operator new
// 0x5136b8 / operator delete 0x513710 in game, malloc/free in the native test).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ltn {

struct MsvcString {             // 0x20 bytes
	union { char buf[16]; char* ptr; } text; // inline when cap < 16
	uint64_t size;              // +0x10
	uint64_t cap;               // +0x18 (15 = inline)
};
static_assert(sizeof(MsvcString) == 0x20, "MSVC std::string");
struct MsvcVector {             // 0x18 bytes
	uint8_t* begin;
	uint8_t* end;
	uint8_t* cap;
};
static_assert(sizeof(MsvcVector) == 0x18, "MSVC std::vector");

struct Heap {
	void* (*alloc)(size_t bytes);
	void (*free)(void* p);
};
// MSVC _Allocate / _Deallocate: a block of 0x1000 bytes or more gets 0x27 more, the pointer is
// 32-aligned and the raw pointer is kept just before it (as D2RCore does at 0x3b73e2..0x3b73f7)
void* Allocate(const Heap& heap, size_t bytes);
void  Deallocate(const Heap& heap, void* p, size_t bytes);

std::string Text(const MsvcString& s);
// Is it a list as D2RCore makes it? (begin <= end <= cap, whole strings, each string sane)
bool Sane(const MsvcVector& v, std::string& why);
size_t Count(const MsvcVector& v);

// A list with one more name: the old strings moved in as they are (their text blocks stay), the new
// one at the end. Nothing in `v` is touched.
struct Prepared {
	uint8_t* array  = nullptr;
	size_t   bytes  = 0;   // of the new array (capacity = count)
	size_t   count  = 0;
	char*    text   = nullptr; // the new name's own block (when it is 16 letters or more)
	size_t   textBytes = 0;
};
bool Prepare(const Heap& heap, const MsvcVector& v, const std::string& name, Prepared& out, std::string& why);
// A list without the name at `index` (a removed stat): the others moved in as they are. The removed
// name's own text block is in out.text / out.textBytes: free it (Discard of only that) after the swap.
bool PrepareWithout(const Heap& heap, const MsvcVector& v, size_t index, Prepared& out, std::string& why);
void FreeRemovedText(const Heap& heap, Prepared& p);
void DiscardWithout(const Heap& heap, Prepared& p);
// A list with the name at `index` replaced (a renamed stat). out.text = the NEW name's block (if any);
// the old name's block, freed after the swap with FreeOldText, is in oldText / oldTextBytes.
struct Replaced { Prepared p; char* oldText = nullptr; size_t oldTextBytes = 0; };
bool PrepareReplace(const Heap& heap, const MsvcVector& v, size_t index, const std::string& name, Replaced& out, std::string& why);
void FreeOldText(const Heap& heap, Replaced& r);
void DiscardReplace(const Heap& heap, Replaced& r); // not used: the new array and the new name's block back // a PrepareWithout result not used (the name stays in the old list)
void Discard(const Heap& heap, Prepared& p);       // a prepared list that will not be used
// Under the lock: the list becomes the prepared one. -> the old array and its size, for Release.
void Swap(MsvcVector& v, const Prepared& p, uint8_t*& oldArray, size_t& oldBytes);
void Release(const Heap& heap, uint8_t* oldArray, size_t oldBytes); // after the lock

} // namespace ltn
