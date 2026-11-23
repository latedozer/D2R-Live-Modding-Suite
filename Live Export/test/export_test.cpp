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
	// a made stat past the table's end, a changed one with a typed string key
	ReadStatEdits("478\t1\t13\t13\t-\t-\t-\t-\t-\t-\t-\t2\t12\thitpoints\tmanarecoverybonus\t-\t-\tmanaperlife\txlt_unused_478\r\n"
	              "0\t-\t11\t8\t32\t-\t70\t1\t1\t5382\t5382\t-\t-\t-\t-\t-\t-\t-\tstrength\r\n",
		edits);
	ReadStringKeys("5382\tModStr1a\r\n", edits);
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
	ApplyProperties(props, edits, report);
	const size_t dmg = RowOf(props, "code", "dmg-min");
	Check(props.At(dmg, "func2") == "6" && props.At(dmg, "stat2") == "secondary_mindamage" && props.At(dmg, "func3").empty(), "properties: dmg-min's line 2 is func 6 on secondary_mindamage");
	const size_t before = isc.rows.size();
	ApplyStats(isc, edits, false, report);
	Check(isc.rows.size() == before && !report.problems.empty(), "itemstatcost without padding: a made stat past the end is left out and reported");
	report = Report {};
	ApplyStats(isc, edits, true, report);
	Check(isc.rows.size() == 479 && isc.At(400, "Stat") == "live_unused_400" && isc.At(478, "Stat") == "manaperlife" && isc.At(478, "op") == "2" && isc.At(478, "op base") == "hitpoints"
			&& isc.At(478, "op stat1") == "manarecoverybonus" && isc.At(478, "Save Bits") == "13" && isc.At(478, "Signed") == "1",
		"itemstatcost with padding: empty rows up to 478, then the made stat manaperlife (op 2 of hitpoints to manarecoverybonus, 13 signed bits)");
	Check(isc.At(0, "Stat") == "strength" && isc.At(0, "descpriority") == "70" && isc.At(0, "descstrpos") == "ModStr1a" && report.problems.empty(),
		"a changed game stat keeps its name; its string number goes back to the key (string-keys.txt)");
	std::vector<NewString> strings;
	ApplyAffixes(prefix, kPrefix, edits, isKey, strings, report);
	Check(prefix.At(9, "Name") == "live_magicprefix_9" && prefix.At(9, "mod1code") == "dmg-min" && prefix.At(9, "mod1max") == "10" && prefix.At(9, "itype1") == "scha" && prefix.At(9, "level") == "99"
			&& prefix.At(9, "frequency") == "1" && strings.size() == 1 && strings[0].text == "Tir-forged",
		"magicprefix 9: the made affix with a string of its own for its name");
	Check(prefix.At(135, "Name") == "Stout" && prefix.At(135, "level") == "5", "magicprefix 135 Stout: the changed level, the game's name kept");
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
	Check(ts.find("{ id: 478, was: \"xlt_unused_478\", made: true") != std::string::npos && ts.find("\"Stat\": \"manaperlife\"") != std::string::npos
			&& ts.find("newName: { key: \"live_magicprefix_9\", text: \"Tir-forged\" }") != std::string::npos && ts.find("added: [\n    {\"description\": \"Small Charm <- Eld Rune\"") != std::string::npos,
		"mod.ts holds the edits: the made stat at 478, the made prefix's name, the new recipe");
	// what the tables should be after it, for run_d2rmm.mjs to compare
	Spill(work + "want-itemstatcost.txt", FormatTsv(isc));
	Spill(work + "want-magicprefix.txt", FormatTsv(prefix));
	Spill(work + "want-properties.txt", FormatTsv(props));
	Spill(work + "want-cubemain.txt", FormatTsv(cube));
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
