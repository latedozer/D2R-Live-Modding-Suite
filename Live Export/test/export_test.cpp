// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// export_core natively, on the game's own tables (vanilla text): edits in the Live Suite's file
// formats are written into the tables; the D2RMM mod made of the same edits is written for
// test/run_d2rmm.mjs, which runs it against the same tables and writes what it made.
// usage: export_test <excel folder> <work folder>
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
static Tsv Table(const std::string& path) {
	Tsv t;
	ParseTsv(Slurp(path), t);
	return t;
}
static size_t RowOf(const Tsv& t, const std::string& column, const std::string& value) {
	const int c = t.Column(column);
	for (size_t r = 0; r < t.rows.size(); ++r) {
		if (c >= 0 && t.rows[r][static_cast<size_t>(c)] == value) {
			return r;
		}
	}
	return static_cast<size_t>(-1);
}

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: export_test <excel folder> <work folder>\n");
		return 2;
	}
	const std::string excel = std::string(argv[1]) + "/", work = std::string(argv[2]) + "/";
	Tsv props = Table(excel + "Properties.txt"), isc = Table(excel + "ItemStatCost.txt"), prefix = Table(excel + "MagicPrefix.txt"), cube = Table(excel + "cubemain.txt");
	const Tsv cubeGame = cube;
	Check(props.rows.size() > 200 && isc.rows.size() > 300 && prefix.rows.size() > 600 && cube.rows.size() > 200, "the game's tables are read");
	{
		Tsv again;
		ParseTsv(FormatTsv(isc), again);
		Check(again.headers == isc.headers && again.rows == isc.rows, "a table written out reads back the same");
	}

	// the edit files, as the plugins write them
	Edits edits;
	std::string dmgLine = "dmg-min";
	for (int i = 0; i < kPropCells; ++i) {
		dmgLine += "\t" + std::string(i == 0 ? "5" : i == 4 ? "6" : i == 1 ? "mindamage" : i == 5 ? "secondary_mindamage" : "-");
	}
	auto bad = ReadPropertyEdits("# comment\r\n" + dmgLine + "\r\nbroken\tline\r\n", edits);
	Check(edits.props.size() == 1 && bad.size() == 1, "property-edits.txt is read (a broken line is reported)");
	{ // a property made in Live Properties (this session: on a spare row), one made earlier without edited cells
		std::string mine = "my-prop";
		for (int i = 0; i < kPropCells; ++i) {
			mine += "\t" + std::string(i == 0 ? "17" : i == 1 ? "strength" : "-");
		}
		ReadPropertyEdits(mine + "\r\n", edits);
		ReadMadeProperties("# made properties\r\nmy-prop\tlive_free_p1\r\nbare-prop\tbare-prop\r\nmy-prop\tmy-prop\r\n", edits);
		Check(edits.madeProps == std::vector<std::string> { "my-prop", "bare-prop" }, "made-properties.txt is read: the codes in their order, each once");
	}
	ReadPropertyOff("# switched off\r\n#\r\nac-hth \t\r\n\r\ndmg-min\r\nac-hth\r\n", edits);
	Check(edits.propsOff == std::vector<std::string> { "ac-hth", "dmg-min" }, "property-off.txt is read: comments and blank lines skipped, trailing spaces cut, each code once");
	// a made stat (its line holds the stale number 478; the game has it at 368, right after the last
	// row), a changed one with a typed string key
	ReadStatEdits("478\t1\t13\t13\t-\t-\t-\t-\t-\t-\t-\t2\t12\thitpoints\tmanarecoverybonus\t-\t-\tmanaperlife\txlt_unused_478\r\n"
	              "0\t-\t11\t8\t32\t-\t70\t1\t1\t5382\t5382\t-\t-\t-\t-\t-\t-\t-\tstrength\r\n",
		edits);
	ReadStringKeys("5382\tModStr1a\r\n", edits);
	edits.liveProps    = LiveProps::ok; // what Live Properties' listMade answers
	edits.madeStatRows = { { "manaperlife", 368 } };
	edits.madePropRows = { { "bare-prop", 303 }, { "my-prop", 302 } };
	// Live Tables: manaperlife grown with 20 save bits (the stat card's line says 13: the line wins, as
	// Live Properties edited the row after the growth), a stat grown with no card line, a property
	// grown with its cells; energy disabled; Live Properties' affix-off.txt: Savage (17) and Stout (135, also edited)
	edits.liveTables = LiveTables::ok;
	bad = ReadGrownRows("# Live Tables\nitemstatcost\tv=123\tn=389\tStat=manaperlife\tSave Bits=20\tadd=7\n"
	                    "itemstatcost\tv=123\tn=390\tStat=grown_only\tSigned=1\tSave Bits=9\tSend Bits=9\n"
	                    "properties\tv=123\tn=299\tcode=grown-prop\tfunc1=17\tstat1=grown_only\n"
	                    "broken line\n",
		edits);
	Check(edits.grown.size() == 3 && bad.size() == 1 && edits.grown[0].name == "manaperlife" && edits.grown[2].name == "grown-prop" && edits.grown[1].cells.size() == 4,
		"grown-rows.txt is read: name from Stat / code, v= and n= left out (the game's numbers come from Live Tables' find)");
	edits.grown[0].number = 368, edits.grown[1].number = 369, edits.grown[2].number = 304; // what find() answers
	bad = ReadDisabledRows("# Live Tables\nitemstatcost\tv=123\tenergy\nnonsense\n", edits);
	Check(edits.statsOff.size() == 1 && edits.statsOff[0].name == "energy" && bad.size() == 1, "disabled-rows.txt is read");
	bad = ReadAffixOff("# Live Properties\r\n# one a line\r\nprefix 17\r\nprefix 135 \r\nprefix x\r\n", edits);
	edits.affixesOff[0].was = "Savage", edits.affixesOff[1].was = "Stout";
	Check(edits.affixesOff.size() == 2 && edits.affixesOff[1].kind == kPrefix && edits.affixesOff[1].index == 135 && bad.size() == 1, "affix-off.txt is read (\"prefix 17\" a line)");
	// a made prefix in the empty row 9, a changed one (135 Stout: level)
	std::string made = "prefix\t9\t-\tTir-forged\t1\t-\t99\t-\t-\t1\t-\t-\t-\t-\tdmg-min\t-\t5\t10";
	for (int i = 0; i < 8; ++i) {
		made += "\t-";
	}
	made += "\tscha";
	for (int i = 0; i < 11; ++i) {
		made += "\t-";
	}
	std::string stout = "prefix\t135\tStout\tStout\t1\t-\t5\t-\t1\t4\t-\t-\t-\t-\tac\t-\t1\t1";
	for (int i = 0; i < 8; ++i) {
		stout += "\t-";
	}
	stout += "\tscha";
	for (int i = 0; i < 11; ++i) {
		stout += "\t-";
	}
	bad = ReadAffixEdits(made + "\r\n" + stout + "\r\n", edits);
	Check(edits.stats.size() == 2 && edits.affixes.size() == 2 && bad.empty(), "stat-edits.txt and affix-edits.txt are read");
	// the cube list: the game's own without its second recipe, with one new one at the end
	Tsv recipes = cubeGame;
	recipes.rows.erase(recipes.rows.begin() + 1);
	std::vector<std::string> fresh(recipes.headers.size());
	fresh[static_cast<size_t>(recipes.Column("description"))] = "Small Charm <- Eld Rune";
	fresh[static_cast<size_t>(recipes.Column("enabled"))]     = "1";
	fresh[static_cast<size_t>(recipes.Column("numinputs"))]   = "1";
	fresh[static_cast<size_t>(recipes.Column("input 1"))]     = "r02";
	fresh[static_cast<size_t>(recipes.Column("output"))]      = "cm1,mag,pre=9";
	fresh[static_cast<size_t>(recipes.Column("lvl"))]         = "99";
	recipes.rows.push_back(fresh);
	edits.recipes = recipes;

	const auto isKey = [](const std::string& word) { return word == "Stout" || word == "ModStr1a"; };

	// ---- into the tables (the .mpq export)
	Report report;
	TakeMadeRows(edits, report);
	Check(report.problems.empty() && edits.stats.size() == 3 && edits.stats[0].id == 368 && edits.stats[0].baseId == -1 && !edits.stats[0].grownCells.empty() && edits.stats[2].name == "grown_only"
			&& !edits.stats[2].lined && edits.madeProps == std::vector<std::string> { "my-prop", "bare-prop", "grown-prop" },
		"made rows: the made stat at its number in the game (368, not its line's 478), a grown stat without a card line carried; made properties in the game's row order");
	ApplyProperties(props, edits, report);
	const size_t dmg = RowOf(props, "code", "dmg-min");
	const size_t gameRows = Table(excel + "Properties.txt").rows.size();
	Check(props.rows.size() == gameRows + 3 && props.At(gameRows + 2, "code") == "grown-prop" && props.At(gameRows + 2, "func1") == "17" && props.At(gameRows + 2, "stat1") == "grown_only"
			&& props.At(gameRows, "code") == "my-prop" && props.At(gameRows, "func1") == "17" && props.At(gameRows, "stat1") == "strength"
			&& props.At(gameRows + 1, "code") == "bare-prop" && props.At(gameRows + 1, "func1").empty(),
		"made properties are new rows at the end of properties.txt, in their order, with their cells from property-edits.txt / grown-rows.txt");
	Check(CouldNotCarry(Report {}) == "could not carry: nothing" && CouldNotCarry(Report { {}, { "a", "b" } }) == "could not carry: a; b", "the report's last line: could not carry: ... / nothing");
	const size_t hth = RowOf(props, "code", "ac-hth");
	Check(props.At(dmg, "func1").empty() && props.At(dmg, "func2").empty() && props.At(dmg, "stat2") == "secondary_mindamage" && props.At(dmg, "func3").empty()
			&& hth < props.rows.size() && props.At(hth, "func1").empty() && !props.At(hth, "stat1").empty(),
		"properties: dmg-min keeps its edited stats (line 2 on secondary_mindamage); dmg-min and ac-hth switched off have every func empty, their stats kept");
	{
		Tsv    baseIsc = isc;
		Report r;
		ApplyStats(baseIsc, edits, true, r);
		Check(baseIsc.rows.size() == isc.rows.size() && r.problems.size() == 2 && r.problems[0].find("not in these tables") != std::string::npos && r.problems[1].find("grown_only: not in these tables") != std::string::npos, "base\\itemstatcost: a made stat listMade has not in data version 2 is left out of it");
	}
	ApplyStats(isc, edits, false, report);
	Check(isc.At(368, "Stat") == "manaperlife" && isc.At(368, "*ID") == "368" && isc.At(368, "op") == "2" && isc.At(368, "op base") == "hitpoints"
			&& isc.At(368, "op stat1") == "manarecoverybonus" && isc.At(368, "Save Bits") == "13" && isc.At(368, "Signed") == "1",
		"itemstatcost: the made stat manaperlife is the next row, 368 (op 2 of hitpoints to manarecoverybonus, 13 signed bits)");
	Check(isc.At(368, "Save Bits") == "13" && isc.At(368, "Add") == "7",
		"a made stat whose save bits were edited (grown with 20, the card says 13) is exported with the edited 13; a grown cell the card has not (add=7, any case) is kept");
	Check(isc.rows.size() == 370 && isc.At(369, "Stat") == "grown_only" && isc.At(369, "Save Bits") == "9" && isc.At(369, "*ID") == "369" && isc.At(369, "Signed") == "1",
		"a stat grown in Live Tables with no card line: the next row, 369, with its grown cells");
	const size_t energy = RowOf(isc, "Stat", "energy");
	Check(energy == 1 && isc.At(1, "op").empty() && isc.At(1, "op base").empty() && isc.At(1, "op stat1").empty() && isc.At(1, "descfunc").empty() && isc.At(1, "dgrp").empty()
			&& isc.At(1, "itemevent1").empty() && isc.At(1, "Save Bits") == Table(excel + "ItemStatCost.txt").At(1, "Save Bits"),
		"disabled energy: op, its links, description and item events empty; saved as before");
	Check(isc.At(0, "Stat") == "strength" && isc.At(0, "descpriority") == "70" && isc.At(0, "descstrpos") == "ModStr1a" && report.problems.empty(),
		"a changed game stat keeps its name; its string number goes back to the key (string-keys.txt)");
	std::vector<NewString> strings;
	ApplyAffixes(prefix, kPrefix, edits, isKey, strings, report);
	Check(prefix.At(9, "Name") == "live_magicprefix_9" && prefix.At(9, "mod1code") == "dmg-min" && prefix.At(9, "mod1max") == "10" && prefix.At(9, "itype1") == "scha" && prefix.At(9, "level") == "99"
			&& prefix.At(9, "frequency") == "1" && strings.size() == 1 && strings[0].text == "Tir-forged",
		"magicprefix 9: the made affix with a string of its own for its name");
	Check(prefix.At(135, "Name") == "Stout" && prefix.At(135, "level") == "5", "magicprefix 135 Stout: the changed level, the game's name kept");
	Check(prefix.At(17, "spawnable") == "0" && prefix.At(17, "frequency") == "0" && prefix.At(135, "spawnable") == "0" && prefix.At(135, "frequency") == "0" && !prefix.At(17, "mod1code").empty(),
		"disabled affixes Savage and Stout (edited too): spawnable 0, frequency 0, their mods kept");
	const std::string json = AddStrings("[{\"id\":10,\"Key\":\"a\"},{\"id\":97030,\"Key\":\"b\"}]", strings, HighestStringId("[{\"id\":10},{\"id\":97030}]") + 1);
	Check(json.find("{\"id\":97031,\"Key\":\"live_magicprefix_9\",\"enUS\":\"Tir-forged\"") != std::string::npos && json.back() == ']', "the name is added to the strings file after the highest id");
	Check(ApplyCube(cube, recipes, report, "cubemain") && cube.rows.size() == cubeGame.rows.size() && cube.At(cube.rows.size() - 1, "output") == "cm1,mag,pre=9" && cube.rows[1] == cubeGame.rows[2],
		"cubemain: the cube list as it is in the game");
	Tsv nothing;
	Check(!ApplyCube(cube, nothing, report, "cubemain"), "an empty recipe list leaves the cube as the game's");

	// ---- the D2RMM mod
	Report      d2rmm;
	const std::string ts = D2rmmTs(edits, cubeGame, Tsv {}, isKey, d2rmm);
	Spill(work + "mod.ts", ts);
	Spill(work + "mod.json", D2rmmJson("Test Export", "1.0", "made by the test"));
	Check(ts.find("{ id: 368, was: \"xlt_unused_478\", made: true") != std::string::npos && ts.find("\"Stat\": \"manaperlife\"") != std::string::npos && ts.find("live_unused_") == std::string::npos
			&& ts.find("newName: { key: \"live_magicprefix_9\", text: \"Tir-forged\" }") != std::string::npos && ts.find("added: [\n    {\"description\": \"Small Charm <- Eld Rune\"") != std::string::npos,
		"mod.ts holds the edits: the made stat at 368, the made prefix's name, the new recipe; no filler rows");
	// what the tables should be after it, for run_d2rmm.mjs to compare
	Spill(work + "want-itemstatcost.txt", FormatTsv(isc));
	Spill(work + "want-magicprefix.txt", FormatTsv(prefix));
	Spill(work + "want-properties.txt", FormatTsv(props));
	Spill(work + "want-cubemain.txt", FormatTsv(cube));
	{ // the game run of 2026-11-25: Exalted packed to 389 stats (0-388); stat-edits.txt still holds 477/478
		std::string cells;
		for (int i = 0; i < kStatCells; ++i) {
			cells += i == 0 ? "\t1" : i == 1 || i == 2 ? "\t13" : i == 8 || i == 9 ? "\t5382" : "\t-";
		}
		const std::string lines = "477" + cells + "\tstat477\txlt_unused_477\r\n478" + cells + "\tstat478\txlt_unused_478\r\n";
		std::string       table = "Stat\t*ID\tSigned\tSend Bits\tSave Bits\tdescstrpos\tdescstrneg\t*eol\r\n";
		for (int n = 0; n < 389; ++n) {
			table += "s" + std::to_string(n) + "\t" + std::to_string(n) + "\t\t\t\t\t\t0\r\n";
		}
		Spill(work + "packed-itemstatcost.txt", table);
		const auto run = [&](LiveProps lp, std::vector<MadeRow> rows, Tsv& isc, Report& r, std::vector<std::string> made = {}) {
			Edits e;
			ReadStatEdits(lines, e);
			e.liveProps    = lp;
			e.madeStatRows = rows;
			e.madeProps    = made;
			e.noString     = { 5382 };
			ParseTsv(table, isc);
			TakeMadeRows(e, r);
			ApplyStats(isc, e, false, r);
			return e;
		};
		const auto has = [](const Report& r, const char* words) {
			for (const std::string& p : r.problems) {
				if (p.find(words) != std::string::npos) {
					return true;
				}
			}
			return false;
		};
		Tsv    isc;
		Report r;
		Edits  e = run(LiveProps::ok, { { "stat477", 389 }, { "stat478", 390 } }, isc, r);
		bool   filler = false;
		for (const auto& row : isc.rows) {
			filler = filler || IsFillerName(row[0]);
		}
		Check(isc.rows.size() == 391 && isc.At(389, "Stat") == "stat477" && isc.At(389, "*ID") == "389" && isc.At(390, "Stat") == "stat478" && isc.At(390, "*eol") == "0" && isc.At(390, "Save Bits") == "13"
				&& isc.At(389, "descstrpos").empty() && isc.At(390, "descstrneg").empty() && !filler && r.problems.empty(),
			"listMade 389/390 on a 389-row table: the two made stats are rows 389 and 390, no filler row; string 5382 (the game's 'no string') leaves the cell empty, no problem reported");
		Report d;
		const std::string ts = D2rmmTs(e, Tsv {}, Tsv {}, [](const std::string&) { return false; }, d);
		Spill(work + "mod-packed.ts", ts);
		Spill(work + "want-packed-itemstatcost.txt", FormatTsv(isc));
		Check(ts.find("{ id: 389, was: \"xlt_unused_477\", made: true") != std::string::npos && ts.find("{ id: 390,") != std::string::npos && ts.find("5382") == std::string::npos && d.problems.empty(),
			"mod.ts of the same: stats 389 and 390, string 5382 not written, nothing reported");
		r = Report {};
		run(LiveProps::ok, { { "stat477", 389 }, { "stat478", 395 } }, isc, r);
		Check(isc.rows.size() == 390 && isc.At(389, "Stat") == "stat477" && has(r, "stat478 is 395 in the game, but this table's next row is 390; left out (Live Export writes no filler rows)"),
			"a made stat whose number is not the next row is refused with the reason, not padded");
		r = Report {};
		run(LiveProps::ok, { { "stat477", 389 }, { "stat999", 390 } }, isc, r, { "my-prop" });
		Check(isc.rows.size() == 390 && has(r, "made stat stat478: not in the game this start") && has(r, "stat999 (390) is in the game, but stat-edits.txt has no line") && has(r, "made property my-prop: not in the game this start"),
			"a made stat or property listMade does not list is left out ('not in the game this start'); a listed one without cells is reported");
		r = Report {};
		run(LiveProps::missing, {}, isc, r, { "my-prop" });
		Check(isc.rows.size() == 389 && has(r, "made stat stat477: left out - neither Live Tables nor Live Properties") && has(r, "made property my-prop: left out") && has(r, "made stats and properties: none in the export - neither Live Tables nor Live Properties"),
			"Live Properties and Live Tables missing: no made rows, the report says why");
		r = Report {};
		run(LiveProps::old, {}, isc, r);
		Check(isc.rows.size() == 389 && has(r, "none in the export - neither Live Tables nor Live Properties"), "Live Properties without listMade (API version 1), no Live Tables: no made rows, the report says so");
		{ // renamed in Live Tables (its files follow, the card's line not yet): the line at the grown row's number
			Edits e2;
			ReadStatEdits("389" + cells + "\tstat477\tstat477\r\n", e2);
			e2.liveTables = LiveTables::ok;
			e2.noString   = { 5382 };
			e2.grown      = { { true, "fury", 389, 389, { { "Stat", "fury" }, { "Save Bits", "13" } } } };
			Tsv t;
			ParseTsv(table, t);
			Report r2;
			TakeMadeRows(e2, r2);
			ApplyStats(t, e2, false, r2);
			bool said = false;
			for (const std::string& d : r2.done) {
				said = said || d.find("stat477 is fury now (renamed)") != std::string::npos;
			}
			Check(t.rows.size() == 390 && t.At(389, "Stat") == "fury" && t.At(389, "Signed") == "1" && said && r2.problems.empty(),
				"a stat renamed in Live Tables: exported under its name now (fury), with the card's cells; the report says so");
		}
		Tsv props;
		ParseTsv("code\tfunc1\r\nac\t1\r\nlive_free_p0\t\r\nlive_free_p1\t\r\n", props);
		ApplyProperties(props, Edits {}, r);
		Check(props.rows.size() == 1, "Live Properties' spare live_free_p rows at the properties table's end are left out");
	}
	{ // the tab's buttons: every label fits its slots
		bool        fit = LabelFits("Save changes", 1) && !LabelFits("Save 5 changes", 1) && LabelFits("Save 5 changes", 2);
		std::string long_;
		for (const ActionButton& b : kExportButtons) {
			if (!LabelFits(b.text, b.slots)) {
				fit = false;
				long_ += std::string(" '") + b.text + "'";
			}
		}
		Check(fit, "every button label of the tab fits its slots (at most 12 letters in one, 28 in two)" + long_);
	}
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
