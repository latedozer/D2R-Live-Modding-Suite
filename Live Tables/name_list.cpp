// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "name_list.h"

#include <cstring>

namespace ltn {

void* Allocate(const Heap& heap, size_t bytes) {
	if (bytes == 0) return nullptr;
	if (bytes < 0x1000) return heap.alloc(bytes);
	uint8_t* raw = static_cast<uint8_t*>(heap.alloc(bytes + 0x27));
	if (raw == nullptr) return nullptr;
	uint8_t* p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + 0x27) & ~uintptr_t(0x1f));
	reinterpret_cast<void**>(p)[-1] = raw;
	return p;
}

void Deallocate(const Heap& heap, void* p, size_t bytes) {
	if (p == nullptr) return;
	if (bytes >= 0x1000) p = reinterpret_cast<void**>(p)[-1];
	heap.free(p);
}

std::string Text(const MsvcString& s) {
	const char* t = s.cap >= 16 ? s.text.ptr : s.text.buf;
	return t != nullptr ? std::string(t, size_t(s.size)) : std::string();
}

size_t Count(const MsvcVector& v) { return v.begin == nullptr ? 0 : size_t(v.end - v.begin) / sizeof(MsvcString); }

bool Sane(const MsvcVector& v, std::string& why) {
	if (v.begin == nullptr && v.end == nullptr && v.cap == nullptr) return true; // empty
	if (v.begin == nullptr || v.end < v.begin || v.cap < v.end || (v.end - v.begin) % sizeof(MsvcString) != 0 || (v.cap - v.begin) % sizeof(MsvcString) != 0) {
		why = "D2RCore's stat-name list is not laid out as Live Tables knows it";
		return false;
	}
	for (const uint8_t* at = v.begin; at < v.end; at += sizeof(MsvcString)) {
		const auto* s = reinterpret_cast<const MsvcString*>(at);
		if (s->cap < 15 || s->size > s->cap || s->size > 0x1000 || (s->cap >= 16 && s->text.ptr == nullptr)) {
			why = "a name in D2RCore's stat-name list is not a string as Live Tables knows it";
			return false;
		}
	}
	return true;
}

bool Prepare(const Heap& heap, const MsvcVector& v, const std::string& name, Prepared& out, std::string& why) {
	out = Prepared();
	if (!Sane(v, why)) return false;
	const size_t old = Count(v);
	out.count = old + 1;
	out.bytes = out.count * sizeof(MsvcString);
	out.array = static_cast<uint8_t*>(Allocate(heap, out.bytes));
	if (out.array == nullptr) { why = "D2RCore's heap gave no memory"; return false; }
	if (old != 0) memcpy(out.array, v.begin, old * sizeof(MsvcString)); // MSVC strings move bit for bit
	MsvcString s {};
	s.size = name.size();
	if (name.size() < 16) {
		s.cap = 15;
		memcpy(s.text.buf, name.data(), name.size());
		s.text.buf[name.size()] = '\0';
	} else {
		s.cap = name.size() | 15;           // as basic_string's growth for a new string
		if (s.cap < 0x16) s.cap = 0x16;
		out.textBytes = s.cap + 1;
		out.text = static_cast<char*>(Allocate(heap, out.textBytes));
		if (out.text == nullptr) { Discard(heap, out); why = "D2RCore's heap gave no memory"; return false; }
		memcpy(out.text, name.data(), name.size());
		out.text[name.size()] = '\0';
		s.text.ptr = out.text;
	}
	memcpy(out.array + old * sizeof(MsvcString), &s, sizeof s);
	return true;
}

bool PrepareWithout(const Heap& heap, const MsvcVector& v, size_t index, Prepared& out, std::string& why) {
	out = Prepared();
	if (!Sane(v, why)) return false;
	const size_t old = Count(v);
	if (index >= old) { why = "the name is not in D2RCore's list"; return false; }
	out.count = old - 1;
	out.bytes = out.count * sizeof(MsvcString);
	if (out.count != 0) {
		out.array = static_cast<uint8_t*>(Allocate(heap, out.bytes));
		if (out.array == nullptr) { why = "D2RCore's heap gave no memory"; return false; }
		memcpy(out.array, v.begin, index * sizeof(MsvcString));
		memcpy(out.array + index * sizeof(MsvcString), v.begin + (index + 1) * sizeof(MsvcString), (old - index - 1) * sizeof(MsvcString));
	}
	const auto* gone = reinterpret_cast<const MsvcString*>(v.begin) + index;
	if (gone->cap >= 16) { out.text = gone->text.ptr; out.textBytes = gone->cap + 1; } // freed after the swap
	return true;
}

void FreeRemovedText(const Heap& heap, Prepared& p) {
	Deallocate(heap, p.text, p.textBytes);
	p.text = nullptr; p.textBytes = 0;
}

bool PrepareReplace(const Heap& heap, const MsvcVector& v, size_t index, const std::string& name, Replaced& out, std::string& why) {
	out = Replaced();
	if (!Sane(v, why)) return false;
	const size_t count = Count(v);
	if (index >= count) { why = "the name is not in D2RCore's list"; return false; }
	MsvcVector empty {};
	Prepared one;
	if (!Prepare(heap, empty, name, one, why)) return false;         // the new string, made as one
	out.p.count = count;
	out.p.bytes = count * sizeof(MsvcString);
	out.p.array = static_cast<uint8_t*>(Allocate(heap, out.p.bytes));
	if (out.p.array == nullptr) { Discard(heap, one); why = "D2RCore's heap gave no memory"; return false; }
	memcpy(out.p.array, v.begin, out.p.bytes);
	memcpy(out.p.array + index * sizeof(MsvcString), one.array, sizeof(MsvcString));
	out.p.text = one.text; out.p.textBytes = one.textBytes;
	Deallocate(heap, one.array, one.bytes);                            // the 1-string array only
	const auto* gone = reinterpret_cast<const MsvcString*>(v.begin) + index;
	if (gone->cap >= 16) { out.oldText = gone->text.ptr; out.oldTextBytes = gone->cap + 1; }
	return true;
}

void FreeOldText(const Heap& heap, Replaced& r) {
	Deallocate(heap, r.oldText, r.oldTextBytes);
	r.oldText = nullptr; r.oldTextBytes = 0;
}

void DiscardReplace(const Heap& heap, Replaced& r) {
	Deallocate(heap, r.p.text, r.p.textBytes);
	Deallocate(heap, r.p.array, r.p.bytes);
	r = Replaced();
}

void DiscardWithout(const Heap& heap, Prepared& p) {
	Deallocate(heap, p.array, p.bytes);
	p = Prepared();
}

void Discard(const Heap& heap, Prepared& p) {
	Deallocate(heap, p.text, p.textBytes);
	Deallocate(heap, p.array, p.bytes);
	p = Prepared();
}

void Swap(MsvcVector& v, const Prepared& p, uint8_t*& oldArray, size_t& oldBytes) {
	oldArray = v.begin;
	oldBytes = size_t(v.cap - v.begin);
	v.begin = p.array;
	v.end   = p.array + p.bytes;
	v.cap   = p.array + p.bytes;
}

void Release(const Heap& heap, uint8_t* oldArray, size_t oldBytes) {
	Deallocate(heap, oldArray, oldBytes); // only the array: its strings live on in the new one
}

} // namespace ltn
