// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
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

#include <cstddef>
#include <cstdint>

namespace modbook {

constexpr uint32_t kApiVersion  = 1; // stays 1: additions are found by size
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

	// Mod Book 0.8.0 and later (an older Mod Book ignores them). Asked by the Legend on the other
	// side when the player clicks an entry's Insert: the tab puts the text into its cell being edited.
	int(__cdecl* editing)(void* user) noexcept;                  // 1: a cell is being edited now (the Legend shows Insert); nullptr = never
	int(__cdecl* insert)(void* user, const char* text) noexcept; // 1: taken; 0: no cell is being edited (the Legend says so)
};

// ---------------------------------------------------------------- the Legend (Mod Book 0.8.0)
//
// One "Legend" tab on the right side that Mod Book owns. Plugins add sections to it instead of
// making their own legend tabs. Its section buttons are along the top; the search covers every
// section (each result tagged with its section); a list row is one line "code - meaning"; a click
// opens a card with the detail and, when the tab on the other side is editing a cell, Insert.
// The Legend follows the tab shown on the other side: it opens the first section that `follows` it.
//
// Mod Book asks for the entries (count/get) when it needs them, on the UI thread, and copies the
// texts at once: the strings given only need to live until get returns. get is never called from
// inside addLegend or legendChanged (those only take note; any thread, call them with no lock held).
// After the entries change, call legendChanged(section) and they are asked for again. A Tab's
// editing and insert are called on the UI thread.

struct LegendEntry {
	const char* code;    // "maxhp", "res-all", "op 2"; shown first in a row, searched
	const char* meaning; // one line: "the most life"
	const char* detail;  // the card's text (more lines); nullptr or "" = the meaning
	const char* insert;  // what Insert puts into the cell being edited; nullptr or "" = no Insert ("maxhp", "res-all")
};

struct LegendSection {
	uint32_t    size;    // sizeof(LegendSection)
	const char* id;      // letters and digits; one of its kind ("stats", "props", "recipeinputs")
	const char* title;   // on its button: "Stats", "Properties"
	int         follows; // a tab's handle (addTab): the Legend opens this section while that tab is shown. 0: none
	void*       user;
	// how = 1: the "How it works" entries, shown above the list (units such as x256, limits, how op
	// works) - a few; how = 0: the list itself. count gives how many; get fills `entry` for index
	// 0 .. count-1 (0: there is no such entry).
	int(__cdecl* count)(void* user, int how) noexcept;
	int(__cdecl* get)(void* user, int how, int index, LegendEntry* entry) noexcept;
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

	// Mod Book 0.6.0 and later (check size first, or use SideOf below): the side a tab is on now.
	// The player can drag tabs to the other side, so a tab's side is not always the one it asked for.
	int(__cdecl* sideOf)(int tab) noexcept; // kLeft, kRight; -1: no such tab

	// Mod Book 0.8.0 and later (check with HasRows / HasLegend below).
	//
	// The action row: a tab's own buttons (New, Templates, Back, Save, Discard ...) in rows right
	// under the tab rows, so that every plugin's buttons stand in the same places. Slots 232 wide,
	// 4 to a row from x 82 (82 / 322 / 562 / 802, 8 apart), filled left to right in the order of the
	// calls. A button takes `slots` slots (1 to 4; 0 = 1) and starts at the next free slot; when it
	// does not fit in what is left of the row it starts a new row kActionRowStep lower. Its picture
	// is the widest that fits: 1 slot 232, 2 slots 470, 3 slots 700, 4 slots 742. The first row
	// stands at y kActionY of the tab's places (moved down with everything at y 250 or lower, as the
	// tab rows grow). The answer is the first y under the action rows so far: the tab puts its own
	// list there or lower. color nullptr = gold. Dialogs (confirm, prompt) keep using button().
	// click nullptr: no button - the row ends here, the next button starts a new row (the answer
	// counts that row as used when it holds a button).
	int(__cdecl* action)(Page*, const char* click, const char* text, const char* color, int slots) noexcept;
	// The search box, in the same place and width on every tab (x 210 y 138, its text x 250 660
	// wide). typed: what the player typed ("" shows hint). The tab still gets the keys (keys()).
	void(__cdecl* search)(Page*, const char* typed, const char* hint) noexcept;

	// The Legend (see LegendSection). Any thread.
	int(__cdecl* addLegend)(const LegendSection* section) noexcept; // a handle above 0; 0 = refused. The same id again replaces it
	void(__cdecl* legendChanged)(int section) noexcept;              // its entries are read again
	// Opens the Legend at this section (and at the entry with this code; nullptr: the list's top).
	// With code nullptr while the Legend is on screen showing this section, it closes the Legend
	// instead (a key can toggle it). section 0: the section that follows the tab shown on the left.
	void(__cdecl* showLegend)(int section, const char* code) noexcept;
};

constexpr int kActionY = 262, kActionSlot = 232, kActionGap = 8, kActionsInRow = 4, kActionRowStep = 56;

// The Api has the action row and the search box (Mod Book 0.8.0 and later).
inline bool HasRows(const Api* api) {
	return api != nullptr && api->size >= offsetof(Api, search) + sizeof(api->search) && api->action != nullptr && api->search != nullptr;
}
// The Api has the Legend (Mod Book 0.8.0 and later); without it a plugin may keep its own legend tab.
inline bool HasLegend(const Api* api) {
	return api != nullptr && api->size >= offsetof(Api, showLegend) + sizeof(api->showLegend) && api->addLegend != nullptr;
}

// The side a tab is on now; `asked` (the side it was added to) with a Mod Book from before 0.6.0.
inline int SideOf(const Api* api, int tab, int asked) {
	const int side = api != nullptr && api->size >= offsetof(Api, sideOf) + sizeof(api->sideOf) && api->sideOf != nullptr ? api->sideOf(tab) : -1;
	return side >= 0 ? side : asked;
}

// The Api of the Mod Book plugin loaded in this game, or nullptr.
inline const Api* Find() {
	const HMODULE module = GetModuleHandleW(L"d2rl-mod-book.dll");
	if (module == nullptr) {
		return nullptr;
	}
	const auto get = reinterpret_cast<const Api*(__cdecl*)(uint32_t)>(reinterpret_cast<void*>(GetProcAddress(module, "ModBookGetApi")));
	const Api* api = get != nullptr ? get(kApiVersion) : nullptr;
	return api != nullptr && api->size >= offsetof(Api, sideOf) ? api : nullptr; // a Mod Book from before sideOf is still used
}

} // namespace modbook
