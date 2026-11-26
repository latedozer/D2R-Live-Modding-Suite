// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The export of edits to Exalted Items' own cube rows (agreed with the Exalted Items session).
// Makes, from Exalted Items' cubemain (its generator's output on the game's tables): a recipe
// list with one upgrade, one back-to-elite and one restore recipe changed, one restore recipe
// removed and one recipe of the user's added; then the D2RMM mod and the plain .mpq's cubemain
// of it. check_exalted.sh runs Exalted Items' generator on both and checks what it kept.
// usage: exalted_test <Exalted Items' cubemain> <the game's cubemain> <work folder>
#include "../export_core.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace exporter;

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	g_failed += ok ? 0 : 1;
}
static std::string Slurp(const std::string& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}
static void Spill(const std::string& path, const std::string& text) {
	std::ofstream(path, std::ios::binary) << text;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: exalted_test <Exalted Items' cubemain> <the game's cubemain> <work folder>\n");
		return 2;
	}
	const std::string work = std::string(argv[3]) + "/";
	Tsv               exalted, game;
	ParseTsv(Slurp(argv[1]), exalted);
	ParseTsv(Slurp(argv[2]), game);
	size_t xlt = 0;
	for (size_t r = 0; r < exalted.rows.size(); ++r) {
		xlt += IsExalted(exalted.At(r, "description")) ? 1 : 0;
	}
	Check(xlt > 300, "Exalted Items' cubemain has its recipes (" + std::to_string(xlt) + ")");

	// the key rules, as Exalted Items has them
	Check(ExaltedKey("\"useitem,mod,exc\"", " \"TORS,eli,uni\" ") == "upgrade:tors,eli,uni" && ExaltedKey("useitem,mod,eli", "\"x7f,qty=1\"") == "downgrade:x7f"
			&& ExaltedKey("useitem", "x7f") == "restore:x7f" && IsExalted("[xlt] a") && !IsExalted("[XLT] a") && !IsExalted("[xlt]a"),
		"the keys: upgrade = whole input 1, back to elite / restore = input 1 up to the comma; trimmed, quotes off, lower case; '[xlt] ' exactly");

	// the user's list: Exalted Items' own, with changes
	Tsv         list = exalted;
	std::string upInput, downInput, helmInput, bodyDescription;
	for (size_t r = 0; r < list.rows.size(); ++r) {
		const std::string d = list.At(r, "description");
		if (upInput.empty() && d.rfind("[xlt] Elite", 0) == 0) {
			list.At(r, "input 2") = "r33";
			upInput               = list.At(r, "input 1");
		} else if (downInput.empty() && d.rfind("[xlt] Exalted", 0) == 0 && d.find("-> ") != std::string::npos) {
			list.At(r, "input 2") = "r05";
			downInput             = list.At(r, "input 1");
		} else if (bodyDescription.empty() && d.find("restored: Body armor") != std::string::npos) {
			list.At(r, "description") = bodyDescription = "[xlt] my mod: " + d.substr(6);
		}
	}
	for (size_t r = 0; r < list.rows.size(); ++r) {
		if (list.At(r, "description").find("restored: Helms") != std::string::npos) {
			helmInput = list.At(r, "input 1");
			list.rows.erase(list.rows.begin() + static_cast<long>(r));
			break;
		}
	}
	std::vector<std::string> mine(list.headers.size());
	mine[static_cast<size_t>(list.Column("description"))] = "Small Charm <- Eld Rune";
	mine[static_cast<size_t>(list.Column("enabled"))]     = "1";
	mine[static_cast<size_t>(list.Column("numinputs"))]   = "1";
	mine[static_cast<size_t>(list.Column("input 1"))]     = "r02";
	mine[static_cast<size_t>(list.Column("output"))]      = "cm1,mag,pre=9";
	list.rows.push_back(mine);
	Check(!upInput.empty() && !downInput.empty() && !helmInput.empty() && !bodyDescription.empty(), "one upgrade, one back-to-elite and one restore recipe changed, one restore recipe (Helms) removed");
	Spill(work + "expect.txt", upInput + "\n" + downInput + "\n" + helmInput + "\n" + bodyDescription + "\n");

	// edits to Exalted Items' own stat and property rows are left out
	Edits edits;
	edits.recipes = list;
	std::string row = "495";
	for (int i = 0; i < kStatCells; ++i) {
		row += "\t-";
	}
	ReadStatEdits(row + "\t-\txlt_roll_5\r\n478" + row.substr(3) + "\tmine\txlt_unused_478\r\n", edits);
	std::string prop = "xlt-r1";
	for (int i = 0; i < kPropCells; ++i) {
		prop += "\t-";
	}
	ReadPropertyEdits(prop + "\r\n", edits);
	Report left;
	LeaveOutExaltedRows(edits, left);
	Check(edits.stats.size() == 1 && edits.stats[0].id == 478 && edits.props.empty() && left.problems.size() == 2,
		"edits to xlt_roll_5 (495) and xlt-r1 are left out and reported; a made stat in an xlt_unused_ row stays");
	{ // Exalted Items 1.3.0's stat-ids.txt: its own numbers by the map, made stats in every form carried
		ExaltedIds  ids;
		std::string why;
		std::string map = "# Exalted Items: stat numbers\r\nlayout=packed\r\nmarker=375\r\n";
		for (int k = 0; k < 13; ++k) {
			map += "roll." + std::to_string(k) + "=" + std::to_string(376 + k) + "\r\n";
		}
		map += "made.stat477=389\r\nmade.stat478=390\r\nfree=391\r\nfree=392\r\n";
		const bool read = ReadExaltedIds(map, ids, why) && !ids.legacy && ids.own.size() == 14 && ids.free.size() == 2 && ids.made.at("stat478") == 390;
		ExaltedIds  bad;
		const bool  refused = !ReadExaltedIds("layout=packed\r\nmarker=375\r\nrol.1=3\r\n", bad, why) && why.find("line 3") == 0 && !ReadExaltedIds("marker=375\r\n", bad, why);
		Edits mine;
		const std::string cells = row.substr(3);
		ReadStatEdits("375" + cells + "\t-\txlt_restored\r\n"                // Exalted's marker
		              "380" + cells + "\t-\t-\r\n"                           // a roll's number, no name
		              "389" + cells + "\tstat477\tstat477\r\n"               // made, migrated: was is its name
		              "390" + cells + "\t-\tstat478\r\n"                     // made by the map alone (no made name)
		              "391" + cells + "\tstat391\tlive_free_391\r\n"         // made this session in a free row
		              "7" + cells + "\t-\tmaxhp\r\n"                         // a game stat changed
		              "487" + cells + "\tbig\txlt_unused_487\r\n",           // made in an old filler: kept with the map
			mine);
		Report report;
		LeaveOutExaltedRows(mine, report, &ids);
		std::string kept;
		for (const StatEdit& s : mine.stats) {
			kept += std::to_string(s.id) + (s.made ? "m " : " ");
		}
		Check(read && refused && kept == "389m 390m 391m 7 487m " && report.problems.size() == 2,
			"with stat-ids.txt: Exalted Items' marker and rolls are left out by number; made stats are carried (made name, map, live_free_, xlt_unused_), a game stat stays one (" + kept + why + ")");
		Edits old;
		ReadStatEdits("487" + cells + "\tbig\txlt_unused_487\r\n", old);
		Report oldReport;
		LeaveOutExaltedRows(old, oldReport);
		Check(old.stats.empty() && oldReport.problems.size() == 1, "without stat-ids.txt: made stats past 486 are left out as before");
	}

	// the D2RMM mod (made against Exalted Items' list) and the plain .mpq's cubemain (on the game's)
	edits.liveProps    = LiveProps::ok; // listMade: the made stat right after the game's 368 stats
	edits.madeStatRows = { { "mine", 368 } };
	Report d2rmm;
	TakeMadeRows(edits, d2rmm);
	Spill(work + "mod.ts", D2rmmTs(edits, exalted, Tsv {}, [](const std::string&) { return false; }, d2rmm));
	bool said = false;
	for (const std::string& line : d2rmm.done) {
		said = said || line.find("3 Exalted Items recipes changed, 1 removed (written with enabled 0)") != std::string::npos;
	}
	Check(said, "the D2RMM report: 3 Exalted Items recipes changed, 1 removed");
	Tsv    plain = game;
	Report report;
	ApplyCubePlain(plain, list, exalted, report, "cubemain");
	size_t plainXlt = 0, disabled = 0;
	for (size_t r = 0; r < plain.rows.size(); ++r) {
		if (IsExalted(plain.At(r, "description"))) {
			++plainXlt;
			disabled += plain.At(r, "enabled") == "0" ? 1 : 0;
		}
	}
	Check(plainXlt == 4 && disabled == 1 && plain.rows.size() == game.rows.size() + 4 + 1,
		"the plain .mpq's cube list: the game's recipes, the 3 changed Exalted Items recipes, the removed one with enabled 0, the user's new one (" + std::to_string(plainXlt) + " Exalted rows)");
	Spill(work + "plain-cubemain.txt", FormatTsv(plain));
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
