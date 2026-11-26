// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// stat-edits.txt after made stats got new numbers (the handover to Live Tables). Since 0.10.0
// Live Tables adds every row; this plugin no longer reads or extends the tables' text.
#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace table {

// The first cell (the number) of each moved line becomes its new number and the last cell (the
// game's name for the row) the made name (the cell before it). Comment lines and every other line
// stay as they are, byte for byte.
std::string RenumberEdits(const std::string& file, const std::map<size_t, size_t>& moved);

} // namespace table
