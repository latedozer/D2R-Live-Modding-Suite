// Stand-in for D2RCore.dll: the two cube cell compilers, with the real ones' arguments and
// record places (inputs 10 bytes each from +0x14, outputs 0x5b bytes each from +0x5a).
// What they write is made up but depends on every character of the cell, the slot and the data
// version. A cell containing "zzz" is refused, as the game refuses a code it does not know.
// Like the game's parsers they cut the cell text up in place.
#include <cstdint>
#include <cstring>

static uint32_t Hash(const char* text) {
	uint32_t hash = 2166136261u;
	for (; *text != '\0'; ++text) {
		hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
	}
	return hash;
}

static void CutUp(char* text) {
	for (; *text != '\0'; ++text) {
		if (*text == ',' || *text == '"') {
			*text = '\0';
		}
	}
}

extern "C" __declspec(dllexport) int CompileCubeInputField(uint8_t version, char* text, void* record, int slot, uint64_t, int) {
	if (text == nullptr || record == nullptr || *text == '\0' || slot < 0 || slot >= 7) {
		return 0x7777; // the real one returns whatever is in eax here
	}
	const bool     known = strstr(text, "zzz") == nullptr;
	const uint32_t hash  = Hash(text);
	uint8_t*       at    = static_cast<uint8_t*>(record) + 0x14 + slot * 10;
	if (known) {
		at[0] = 1;
		memcpy(at + 2, &hash, 4);
		at[6] = version;
		at[7] = static_cast<uint8_t>(strlen(text));
	}
	CutUp(text);
	return known ? 1 : 0;
}

extern "C" __declspec(dllexport) int CompileCubeOutputField(uint8_t version, char* text, void* record, int slot, uint64_t, int line) {
	if (text == nullptr || record == nullptr || *text == '\0' || slot < 0 || slot >= 3) {
		return 0x7777;
	}
	const bool     known = strstr(text, "zzz") == nullptr;
	const uint32_t hash  = Hash(text);
	uint8_t*       at    = static_cast<uint8_t*>(record) + 0x5a + slot * 0x5b;
	memcpy(at, &line, 4); // the game stores the line number first, whatever follows
	if (known) {
		at[4] = 8;
		memcpy(at + 6, &hash, 4);
		at[0xa] = version;
		at[0xd] = 0xfc;
	}
	CutUp(text);
	return known ? 1 : 0;
}
