// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Native test of Live Strings' core: names and revisions, the string files written for the loader,
// the kept file and how it is read back at load (the bytes registered as kept.json).
//   core_test <out folder>   (writes the files it made there for run_tests.sh's JSON check)
#include "../strings_core.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_n = 0, g_bad = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_bad; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static ls::Ask A(const std::string& name, const std::string& en) { ls::Ask a; a.name = name; a.lang[0] = en; a.has[0] = true; return a; }

static void Save(const std::string& dir, const std::string& file, const std::string& text) { std::ofstream(dir + "/" + file, std::ios::binary) << text; }

// echo <in.json> <out.json>: every record of a string-only array (Key, 13 languages) added as a name,
// then the kept file written: run_tests.sh compares its texts with the input (real game texts).
static int Echo(const char* in, const char* outPath) {
	std::ifstream f(in, std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::vector<ls::Object> objs;
	std::string why;
	if (!ls::ParseArray(text, objs, why)) { printf("FAIL  echo: %s\n", why.c_str()); return 1; }
	std::vector<ls::Ask> asks;
	for (const ls::Object& o : objs) {
		ls::Ask a;
		a.name = *o.Get("Key");
		for (int i = 0; i < ls::kLanguages; ++i) { a.lang[i] = *o.Get(ls::kCodes[i]); a.has[i] = true; }
		asks.push_back(a);
	}
	ls::Bank bank;
	std::vector<ls::Record> recs;
	if (!bank.Plan(asks, false, recs, why)) { printf("FAIL  echo plan: %s\n", why.c_str()); return 1; }
	bank.Commit(recs);
	std::ofstream(outPath, std::ios::binary) << bank.KeptJson();
	ls::Bank again;
	if (!again.LoadKept(bank.KeptJson(), why) || again.KeptJson() != bank.KeptJson()) { printf("FAIL  echo reload: %s\n", why.c_str()); return 1; }
	printf("ok    %zu real texts x 13 languages written and read back\n", recs.size());
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 4 && std::string(argv[1]) == "echo") return Echo(argv[2], argv[3]);
	const std::string out = argc > 1 ? argv[1] : ".";
	std::string why;

	// names and texts
	CHECK(ls::ValidName("stat391_pos", why));
	CHECK(!ls::ValidName("", why));
	CHECK(!ls::ValidName("has space", why));
	CHECK(!ls::ValidName("colon:x", why));
	CHECK(!ls::ValidName(std::string(49, 'a'), why));
	CHECK(ls::ValidName(std::string(48, 'a'), why));
	CHECK(ls::ValidText("+%d%% to Life", why));
	CHECK(ls::ValidText("\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80", why)); // é 日 😀
	CHECK(!ls::ValidText("\xc3", why));
	CHECK(!ls::ValidText("\xff\xfe", why));
	CHECK(!ls::ValidText(std::string(4097, 'x'), why));
	CHECK(!ls::ValidText(std::string("a\0b", 3), why));

	std::string name;
	int rev = 0;
	CHECK(ls::SplitKey("stat391_pos_r12", name, rev) && name == "stat391_pos" && rev == 12);
	CHECK(ls::SplitKey("a_r2_r3", name, rev) && name == "a_r2" && rev == 3);
	CHECK(!ls::SplitKey("stat391", name, rev));
	CHECK(!ls::SplitKey("x_r0", name, rev));
	CHECK(!ls::SplitKey("x_r01", name, rev));
	CHECK(!ls::SplitKey("_r1", name, rev));
	CHECK(!ls::SplitKey("x_r12345", name, rev));

	// add: revision 1, missing languages = enUS, given languages kept
	ls::Bank bank;
	std::vector<ls::Record> recs;
	ls::Ask two = A("stat391_neg", "-%d%% to Life");
	two.lang[2] = "-%d%% Leben"; two.has[2] = true;  // deDE
	two.lang[6] = "";             two.has[6] = true;  // koKR: given empty (allowed, shows nothing)
	CHECK(bank.Plan({A("stat391_pos", "+%d%% to Life"), two}, false, recs, why));
	CHECK(recs.size() == 2 && recs[0].revision == 1 && recs[1].revision == 1);
	CHECK(recs[0].LocalKey() == "stat391_pos_r1" && recs[0].FullKey() == "live-strings:stat391_pos_r1");
	bool allEn = true;
	for (int i = 0; i < ls::kLanguages; ++i) allEn = allEn && recs[0].texts.lang[i] == "+%d%% to Life";
	CHECK(allEn);
	CHECK(recs[1].texts.lang[2] == "-%d%% Leben" && recs[1].texts.lang[6].empty() && recs[1].texts.lang[11] == "-%d%% to Life");
	CHECK(bank.Entries().empty()); // Plan changes nothing
	const std::string file1 = ls::FileJson(recs);
	Save(out, "add-1.json", file1);
	bank.Commit(recs);
	CHECK(bank.Entries().size() == 2 && bank.Revisions() == 2);

	// refusals (nothing changes)
	CHECK(!bank.Plan({A("stat391_pos", "again")}, false, recs, why) && why.find("already added") != std::string::npos);
	CHECK(!bank.Plan({A("nope", "x")}, true, recs, why) && why.find("never added") != std::string::npos);
	CHECK(!bank.Plan({A("dup", "x"), A("dup", "y")}, false, recs, why) && why.find("twice") != std::string::npos);
	CHECK(!bank.Plan({A("ok", "x"), A("bad name", "y")}, false, recs, why));
	CHECK(!bank.Plan({A("utf", "\xc3")}, false, recs, why) && why.find("enUS") != std::string::npos);
	CHECK(!bank.Plan({}, false, recs, why));
	CHECK(bank.Revisions() == 2);

	// setText: a new revision per edit, older ones kept with their text
	CHECK(bank.Plan({A("stat391_pos", "+%d%% of Life")}, true, recs, why) && recs.size() == 1 && recs[0].revision == 2);
	Save(out, "add-2.json", ls::FileJson(recs));
	bank.Commit(recs);
	CHECK(bank.Plan({A("stat391_pos", "+%d%% of Life added to Mana")}, true, recs, why) && recs[0].revision == 3 && recs[0].LocalKey() == "stat391_pos_r3");
	bank.Commit(recs);
	const ls::Bank::Entry* e = bank.Find("stat391_pos");
	CHECK(e != nullptr && e->revisions.size() == 3 && e->revisions[0].lang[0] == "+%d%% to Life" && e->revisions[2].lang[0] == "+%d%% of Life added to Mana");
	CHECK(bank.Revisions() == 4);
	// a stale Commit (same revision twice) does not add a revision
	bank.Commit(recs);
	CHECK(bank.Revisions() == 4);

	// the string file: what D2RCore reads (Key + 13 languages, no id), read back exactly
	std::vector<ls::Object> objs;
	CHECK(ls::ParseArray(file1, objs, why) && objs.size() == 2);
	CHECK(objs.size() == 2 && objs[0].Get("Key") != nullptr && *objs[0].Get("Key") == "stat391_pos_r1" && objs[0].Get("id") == nullptr && objs[0].fields.size() == 14);
	CHECK(objs.size() == 2 && *objs[1].Get("deDE") == "-%d%% Leben");
	const std::string over = ls::FileJson({ls::Record{"probe", 1, {}}}, "live-strings:");
	CHECK(ls::ParseArray(over, objs, why) && *objs[0].Get("Key") == "live-strings:probe_r1");

	// escaping round trip
	const std::string odd = "quote \" back \\ nl \n tab \t cr \r bell \x07 slash / \xc3\xa9 \xf0\x9f\x98\x80 %d%%";
	ls::Bank eb;
	CHECK(eb.Plan({A("odd", odd)}, false, recs, why));
	eb.Commit(recs);
	CHECK(ls::ParseArray(ls::FileJson(recs), objs, why) && *objs[0].Get("enUS") == odd);
	CHECK(ls::ParseArray("[{\"Key\":\"x_r1\",\"enUS\":\"\\u00e9\\ud83d\\ude00\\/\"}]", objs, why) && *objs[0].Get("enUS") == "\xc3\xa9\xf0\x9f\x98\x80/");
	CHECK(!ls::ParseArray("[{\"Key\":\"x\",\"enUS\":\"\\ud83d\"}]", objs, why));
	CHECK(!ls::ParseArray("[{\"Key\":1}]", objs, why));
	CHECK(!ls::ParseArray("[{\"Key\":\"a\",\"Key\":\"b\"}]", objs, why));
	CHECK(!ls::ParseArray("[{\"Key\":\"a\"}] x", objs, why));
	CHECK(!ls::ParseArray("{}", objs, why));
	CHECK(ls::ParseArray("\xef\xbb\xbf [ ] ", objs, why) && objs.empty());

	// kept file: every revision, read back at load = the same bank, the same bytes registered
	const std::string kept = bank.KeptJson();
	Save(out, "strings.json", kept);
	ls::Bank again;
	CHECK(again.LoadKept(kept, why));
	CHECK(again.KeptJson() == kept);
	CHECK(again.Revisions() == 4 && again.Find("stat391_pos") != nullptr && again.Find("stat391_pos")->revisions.size() == 3);
	CHECK(again.Find("stat391_neg") != nullptr && again.Find("stat391_neg")->revisions[0].lang[2] == "-%d%% Leben");
	// after a restart the next edit continues the revisions
	CHECK(again.Plan({A("stat391_pos", "r4")}, true, recs, why) && recs[0].revision == 4);
	CHECK(again.Plan({A("stat391_pos", "x")}, false, recs, why) == false);
	// a kept file with a BOM and other whitespace (an editor saved it) still loads
	ls::Bank bom;
	CHECK(bom.LoadKept("\xef\xbb\xbf" + kept, why) && bom.KeptJson() == kept);
	// broken kept files are refused whole (the plugin then leaves the file alone and adds nothing)
	ls::Bank broken;
	std::string missingLang = kept;
	missingLang.erase(missingLang.find(", \"zhCN\""), missingLang.find("\"}", missingLang.find(", \"zhCN\"")) + 1 - missingLang.find(", \"zhCN\""));
	CHECK(!broken.LoadKept(missingLang, why) && why.find("zhCN") != std::string::npos && broken.Entries().empty());
	CHECK(!broken.LoadKept("[{\"Key\":\"a_r2\",\"enUS\":\"x\",\"zhTW\":\"\",\"deDE\":\"\",\"esES\":\"\",\"frFR\":\"\",\"itIT\":\"\",\"koKR\":\"\",\"plPL\":\"\",\"esMX\":\"\",\"jaJP\":\"\",\"ptBR\":\"\",\"ruRU\":\"\",\"zhCN\":\"\"}]", why) && why.find("out of order") != std::string::npos);
	CHECK(!broken.LoadKept("[{\"Key\":\"no revision\",\"enUS\":\"x\"}]", why));
	CHECK(!broken.LoadKept("not json", why));
	CHECK(!broken.LoadKept(kept.substr(0, kept.size() / 2), why));
	ls::Bank empty;
	CHECK(empty.LoadKept("[\n]\n", why) && empty.Entries().empty() && empty.KeptJson() == "[\n]\n");

	CHECK(ls::ResourcePath("add-3.json") == "data/local/lng/strings/d2rloader/live-strings/add-3.json");
	CHECK(ls::ResourcePath("kept.json").rfind("data/local/lng/strings/", 0) == 0);

	printf("%s  %d of %d core checks pass\n", g_bad ? "FAIL" : "ok  ", g_n - g_bad, g_n);
	return g_bad ? 1 : 0;
}
