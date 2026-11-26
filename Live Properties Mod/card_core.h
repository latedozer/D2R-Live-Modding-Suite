// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// A card's typing (the Properties, Stats and Affixes cards): cells typed stay pending - green with
// a * - until Save or Discard, however many there are and however the player moves between them
// (Enter, Tab, a click on another cell, Save). Tested natively (test/card_test.cpp).
#pragma once

#include <functional>
#include <map>
#include <string>

namespace card {

using Saved = std::function<std::string(int cell)>; // what the row holds now (nothing typed)

struct Typing {
	int                        cell = -1; // the cell being typed in, -1 = none
	std::string                text;      // what is typed in it
	std::map<int, std::string> pending;   // typed and kept, not saved yet

	// What a cell shows: typed and kept first, else the row's.
	std::string Now(int at, const Saved& saved) const;
	// Leaves the cell being typed in: its text is kept (pending) when it differs from the row's,
	// and a kept one going back to the row's value is no longer pending.
	void Keep(const Saved& saved);
	// A click on a cell: the one being left is kept first, then the new one opens with what it shows.
	void Open(int at, const Saved& saved);
	// Enter (next = -1) or Tab (next = the next cell): keep, then open the next one.
	void Next(int next, const Saved& saved);
	// Save: everything kept, the open cell included; pending is empty after.
	std::map<int, std::string> TakeAll(const Saved& saved);
	void                       Discard();
};

} // namespace card
