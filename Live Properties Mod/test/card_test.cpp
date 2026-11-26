// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The card's typing: the 2026-11-25 21:51 report ("changing a second cell before Save puts the
// first back to its old value") on the stat card of stat477, as the player did it.
#include "../card_core.h"

#include <cstdio>

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

int main() {
	// stat477 as grown: send bits 13 (cell 1), save bits 13 (cell 2), save add 0 (cell 3), op empty (cell 10)
	std::map<int, std::string> row { { 1, "13" }, { 2, "13" }, { 3, "" }, { 10, "" }, { 11, "" } };
	const card::Saved          saved = [&](int cell) { return row.count(cell) != 0 ? row[cell] : std::string(); };
	card::Typing               t;

	// 1. type send bits 6, then CLICK save bits (no Enter), type 6, click op, type 2, then Save
	t.Open(1, saved);
	t.text = "6";
	t.Open(2, saved);
	Check(t.pending.size() == 1 && t.pending.at(1) == "6" && t.text == "13", "a click on the next cell keeps the first one's 6 (pending) and opens save bits with 13");
	t.text = "6";
	t.Open(10, saved);
	t.text = "2";
	Check(t.Now(1, saved) == "6" && t.Now(2, saved) == "6" && t.cell == 10 && t.text == "2", "all three show what was typed before Save (two kept, green with *; op open with 2)");
	const auto sent = t.TakeAll(saved);
	Check(sent.size() == 3 && sent.at(1) == "6" && sent.at(2) == "6" && sent.at(10) == "2" && t.pending.empty() && t.cell < 0, "Save sends all three (the open cell too); nothing left pending");

	// 2. Enter / Tab keep too; Tab opens the next cell with what it shows
	t.Open(1, saved);
	t.text = "7";
	t.Next(2, saved);
	Check(t.pending.at(1) == "7" && t.cell == 2 && t.text == "13", "Tab: 7 kept, save bits opens with 13");
	t.text = "8";
	t.Next(-1, saved);
	Check(t.pending.size() == 2 && t.pending.at(2) == "8" && t.cell < 0, "Enter: 8 kept, no cell open");

	// 3. going back into a kept cell shows the kept text; typing the row's value again un-keeps it
	t.Open(1, saved);
	Check(t.text == "7", "a kept cell opens with what was typed (7), not the row's 13");
	t.text = "13";
	t.Keep(saved);
	Check(t.pending.count(1) == 0 && t.pending.size() == 1, "typed back to the row's 13: no longer pending (nothing to save there)");

	// 4. Discard forgets everything; clicking around without typing keeps nothing
	t.Discard();
	t.Open(3, saved);
	t.Open(11, saved);
	t.Open(1, saved);
	t.Keep(saved);
	Check(t.pending.empty(), "Discard empties it; opening cells without typing keeps nothing");

	printf(g_failed == 0 ? "card: all passed\n" : "card: %d FAILED\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
