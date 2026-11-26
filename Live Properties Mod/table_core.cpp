// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "table_core.h"

#include <cstdlib>
#include <vector>

namespace table {
namespace {

std::vector<std::string> Split(const std::string& line) {
	std::vector<std::string> cells;
	for (size_t from = 0; from <= line.size();) {
		size_t tab = line.find('\t', from);
		tab        = tab == std::string::npos ? line.size() : tab;
		cells.push_back(line.substr(from, tab - from));
		from = tab + 1;
	}
	return cells;
}

} // namespace

std::string RenumberEdits(const std::string& file, const std::map<size_t, size_t>& moved) {
	std::string out;
	size_t      at = 0;
	while (at < file.size()) {
		size_t end = file.find('\n', at);
		end        = end == std::string::npos ? file.size() : end + 1;
		std::string line = file.substr(at, end - at);
		at               = end;
		std::string tail; // the line end, kept
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
			tail.insert(tail.begin(), line.back());
			line.pop_back();
		}
		std::vector<std::string> cells = Split(line);
		char*                    stop  = nullptr;
		const unsigned long      was   = line.empty() || line[0] < '0' || line[0] > '9' ? 0 : strtoul(cells[0].c_str(), &stop, 10);
		const auto               to    = stop != nullptr && *stop == '\0' ? moved.find(was) : moved.end();
		if (to == moved.end() || cells.size() < 3) {
			out += line + tail;
			continue;
		}
		cells[0]                = std::to_string(to->second);
		cells[cells.size() - 1] = cells[cells.size() - 2];
		std::string joined;
		for (size_t i = 0; i < cells.size(); ++i) {
			joined += (i == 0 ? "" : "\t") + cells[i];
		}
		out += joined + tail;
	}
	return out;
}

} // namespace table
