// Mod Book: the two side panels (left: the book, right: the legend) that other plugins put
// tabs into. This header is all a plugin needs; nothing is linked.
//
//   const modbook::Api* book = modbook::Find();          // nullptr: Mod Book is not installed
//   modbook::Tab tab { sizeof(tab), modbook::kLeft, "recipes", "Cube Recipes", user, Draw, Click, Keys, Move, Shown, Leave };
//   int handle = book->addTab(&tab);                      // the Tab is copied
//
// Every callback of a tab runs on the game's UI thread. A tab draws itself only when asked
// (draw); when what it shows has changed it calls redraw(handle), from any thread.
//
// Places are the game's panel pixels (the frame is about 1160 wide; lists use x 82..1072).
// A side with more than one tab shows a row of tab buttons at y 262; everything a tab draws
// at y 250 or lower is then moved down by kStripHeight (twice that when the side has groups and
// the group shown has more than one tab). The pager's line (y 1352) never moves.
#pragma once

#include <windows.h>

#include <cstdint>

namespace modbook {

constexpr uint32_t kApiVersion  = 1;
constexpr int      kLeft        = 0, kRight = 1;
constexpr int      kStripHeight = 50;

// What keys() gets besides letters: one char per key press.
constexpr char kKeyEnter = '\r', kKeyBackspace = '\b', kKeyDelete = '\x7f', kKeyTab = '\t', kKeyBackTab = '\x01', kKeyUp = '\x11', kKeyDown = '\x12', kKeySave = '\x13' /* Ctrl+S */,
               kKeyNew = '\x0e' /* Ctrl+N */, kKeyPaste = '\x16' /* Ctrl+V */, kKeyCopy = '\x03' /* Ctrl+C */, kKeyUndo = '\x1a' /* Ctrl+Z */, kKeyRedo = '\x19' /* Ctrl+Y */;

struct Page; // a page being drawn; only valid inside draw()

struct Tab {
	uint32_t    size;  // sizeof(Tab)
	int         side;  // kLeft or kRight
	const char* id;    // letters and digits; one of its kind on its side
	const char* title; // on the tab's button and as the panel's title
	void*       user;
	void(__cdecl* draw)(void* user, Page* page) noexcept;                    // put the page together with the Api's drawing calls
	void(__cdecl* click)(void* user, const char* what) noexcept;             // a button's or spot's click name; "Escape" for the Esc key
	void(__cdecl* keys)(void* user, const char* typed) noexcept;             // what was typed since the last call
	void(__cdecl* move)(void* user, int pages, int rows, int jump) noexcept; // Page Up/Down and the arrows; the wheel; Home (-1) / End (1)
	void(__cdecl* shown)(void* user, int on) noexcept;                       // 1: it is now the tab on screen; 0: it no longer is
	int(__cdecl* leave)(void* user) noexcept;                                // asked before the tab is closed or left: 0 = not now (the tab shows why)
	const char* group; // nullptr: none. A side whose tabs have groups shows two rows: the groups ("General" first), then the tabs of the group shown
};

struct Api {
	uint32_t size, version;
	int(__cdecl* addTab)(const Tab* tab) noexcept; // a handle above 0; 0 = refused (see mod-book.log)
	void(__cdecl* redraw)(int tab) noexcept;       // any thread
	void(__cdecl* open)(int tab) noexcept;         // any thread: opens the tab's side with this tab
	void(__cdecl* toggle)(int tab) noexcept;       // any thread: closes the side when this tab is on screen, else opens it
	void(__cdecl* close)(int side) noexcept;       // any thread; does not ask leave()
	int(__cdecl* isShown)(int tab) noexcept;

	// drawing: inside draw() only. Text is Latin-1 / ASCII; names must differ within a page.
	void(__cdecl* label)(Page*, const char* name, int x, int y, int width, int height, const char* color, const char* align, int wrap, const char* text) noexcept;
	void(__cdecl* spot)(Page*, const char* name, int x, int y, int width, int height, const char* click, const char* tip) noexcept; // unseen: click and/or hover text
	void(__cdecl* button)(Page*, const char* click, int x, int y, int width, const char* text, const char* color) noexcept;         // widths: 116 188 200 216 220 232 250 300 330 470 480 482 500 512 700 742
	void(__cdecl* image)(Page*, const char* name, int x, int y, const char* filename) noexcept;                                    // one of the game's pictures, "PANEL\\Modals\\InputSlot"
	void(__cdecl* bar)(Page*, int y, uint64_t top, uint64_t shown, uint64_t count) noexcept;                                       // where a list stands
	void(__cdecl* pager)(Page*, int before, int after, const char* text) noexcept;                                                 // the arrows (they call move) and the line between them
	void(__cdecl* title)(Page*, const char* text) noexcept;                                                                        // the panel's title for this page (default: the tab's title)
};

// The Api of the Mod Book plugin loaded in this game, or nullptr.
inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-mod-book.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "ModBookGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= sizeof(Api) ? api : nullptr;
}

} // namespace modbook
