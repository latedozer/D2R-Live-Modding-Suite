// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "card_core.h"

namespace card {

std::string Typing::Now(int at, const Saved& saved) const {
	const auto typed = pending.find(at);
	return typed != pending.end() ? typed->second : saved(at);
}

void Typing::Keep(const Saved& saved) {
	if (cell < 0) {
		return;
	}
	if (text == saved(cell)) {
		pending.erase(cell); // back to what the row holds: nothing to save
	} else {
		pending[cell] = text;
	}
	cell = -1;
	text.clear();
}

void Typing::Open(int at, const Saved& saved) {
	Keep(saved);
	cell = at;
	text = at >= 0 ? Now(at, saved) : std::string();
}

void Typing::Next(int next, const Saved& saved) {
	Keep(saved);
	if (next >= 0) {
		Open(next, saved);
	}
}

std::map<int, std::string> Typing::TakeAll(const Saved& saved) {
	Keep(saved);
	std::map<int, std::string> out;
	out.swap(pending);
	return out;
}

void Typing::Discard() {
	pending.clear();
	cell = -1;
	text.clear();
}

} // namespace card
