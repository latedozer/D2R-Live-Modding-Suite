// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// The save writer against COPIES of real saves: it writes only into build/fix-work (copied from
// the copies in build/saves-copy for every run), never the live files.
// usage: fix_test <folder with copies of .d2s/.d2i> <folder with itemstatcost/armor/weapons/misc.txt> <work folder>
#include "../fix_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace savescan;
constexpr int32_t kDroppedAffix = -1;
namespace fs = std::filesystem;

static int g_failed = 0;
static void Check(bool ok, const std::string& what) {
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
	g_failed += ok ? 0 : 1;
}

static std::string Slurp(const fs::path& path) {
	std::ifstream      in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}
static bool Spit(const fs::path& path, const std::string& bytes) {
	{
		std::ofstream out(path.string() + ".writing", std::ios::binary);
		out << bytes;
		if (!out) {
			return false;
		}
	}
	std::error_code error;
	fs::rename(path.string() + ".writing", path, error);
	return !error;
}

// Files in a work folder; backups into <work>/backup-<n>. plant: flip one bit of the n-th write.
struct DiskIo : FixIo {
	fs::path folder;
	int      backups = 0, writes = 0, plantAt = -1, failBackup = 0, allowWrites = -1, asked = 0;
	std::vector<RestoreEntry> plan;
	bool mayWrite(std::string& why) override {
		if (allowWrites >= 0 && asked++ >= allowWrites) {
			why = "a character is in a game (planted)";
			return false;
		}
		return true;
	}
	bool planned(const std::vector<RestoreEntry>& p, std::string&) override {
		plan = p;
		return true;
	}
	bool backup(const std::vector<FixInput>& files, std::string& out, std::string& why) override {
		if (failBackup != 0) {
			why = "planted backup failure";
			return false;
		}
		const fs::path to = folder / ("backup-" + std::to_string(++backups));
		fs::create_directory(to);
		for (const FixInput& file : files) {
			if (!Spit(to / file.name, file.bytes) || Slurp(to / file.name) != file.bytes) {
				why = "copy failed";
				return false;
			}
		}
		out = to.string();
		return true;
	}
	bool write(const std::string& name, const std::string& bytes, std::string& why) override {
		std::string put = bytes;
		if (writes++ == plantAt && put.size() > 1000) {
			put[put.size() - 300] = static_cast<char>(put[put.size() - 300] ^ 0x10); // a planted difference on disk
		}
		if (!Spit(folder / name, put)) {
			why = "write failed";
			return false;
		}
		return true;
	}
	bool read(const std::string& name, std::string& bytes) override {
		bytes = Slurp(folder / name);
		return true;
	}
};

struct Saves {
	std::vector<FixInput> files;
	void load(const fs::path& folder, const Tables& tables) {
		files.clear();
		std::vector<fs::path> paths;
		for (const auto& entry : fs::directory_iterator(folder)) {
			if (entry.path().extension() == ".d2s" || entry.path().extension() == ".d2i") {
				paths.push_back(entry.path());
			}
		}
		std::sort(paths.begin(), paths.end());
		for (const fs::path& path : paths) {
			files.push_back({ path.filename().string(), path.extension() == ".d2i", Slurp(path), &tables });
		}
	}
};

static FileResult ReadAs(const FixInput& file, const Tables& tables) {
	const auto* data = reinterpret_cast<const uint8_t*>(file.bytes.data());
	return file.stash ? ReadStash(data, file.bytes.size(), tables) : ReadCharacter(data, file.bytes.size(), tables);
}

static void Fresh(const fs::path& from, const fs::path& work) {
	fs::remove_all(work);
	fs::create_directories(work);
	for (const auto& entry : fs::directory_iterator(from)) {
		fs::copy_file(entry.path(), work / entry.path().filename());
	}
}

static bool SameOnDisk(const fs::path& a, const fs::path& b) {
	for (const auto& entry : fs::directory_iterator(a)) {
		if (entry.is_regular_file() && Slurp(entry.path()) != Slurp(b / entry.path().filename())) {
			return false;
		}
	}
	return true;
}

static std::string Counts(const FixOut& out) {
	return std::to_string(out.changed) + " files, " + std::to_string(out.itemsChanged) + " items, " + std::to_string(out.dropped) + " dropped, " + std::to_string(out.renumbered) + " renumbered, "
	       + std::to_string(out.resized) + " re-encoded";
}


// Live Properties' stat-edits.txt: "number<TAB>signed<TAB>send bits<TAB>save bits<TAB>save add<TAB>..." (- = the row's own)
static std::vector<ResizeSpec> StatEdits(const std::string& text, const Tables& tables) {
	std::vector<ResizeSpec> out;
	std::istringstream      in(text);
	std::string             line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> cells;
		std::istringstream       row(line);
		std::string              cell;
		while (std::getline(row, cell, '\t')) {
			cells.push_back(cell);
		}
		if (cells.size() < 5) {
			continue;
		}
		const int number = atoi(cells[0].c_str());
		if (number < 0 || static_cast<size_t>(number) >= tables.stats.size()) {
			continue;
		}
		const StatShape& now = tables.stats[static_cast<size_t>(number)];
		ResizeSpec       spec { static_cast<uint16_t>(number), now.saveBits, now.paramBits, now.saveAdd };
		if (cells[3] != "-") {
			spec.saveBits = static_cast<uint8_t>(atoi(cells[3].c_str()));
		}
		if (cells[4] != "-") {
			spec.saveAdd = atoi(cells[4].c_str());
		}
		out.push_back(spec);
	}
	return out;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: fix_test <saves copy> <excel folder> <work folder>\n");
		return 2;
	}
	const fs::path given = argv[1], excel = argv[2], work = argv[3];
	Tables         tables;
	ShapesFromText(tables, Slurp(excel / "itemstatcost.txt"));
	ReadItemTable(tables, Slurp(excel / "armor.txt"), kArmor);
	ReadItemTable(tables, Slurp(excel / "weapons.txt"), kWeapon);
	ReadItemTable(tables, Slurp(excel / "misc.txt"), kMisc);
	const Tables textOnly = tables; // the mod's text alone (without Live Tables' rows)
	std::vector<MadeRow> grownRows;
	if (argc > 4 && fs::exists(argv[4])) {
		grownRows = MadeStatRows(Slurp(argv[4]));
		for (const MadeRow& row : grownRows) {
			if (tables.stats.size() <= row.number) {
				tables.stats.resize(static_cast<size_t>(row.number) + 1);
			}
			tables.stats[row.number] = row.shape;
		}
		printf("      %zu made rows from Live Tables' grown-rows.txt (copy): table %zu -> %zu rows\n", grownRows.size(), textOnly.stats.size(), tables.stats.size());
	}
	if (argc > 5 && std::filesystem::exists(argv[5])) { // and the shapes Live Properties changed in memory
		const std::vector<ResizeSpec> edits = StatEdits(Slurp(argv[5]), tables);
		std::string                   bad;
		ApplyBefore(tables, edits, bad);
		printf("      %zu stat edits of Live Properties applied (copy of stat-edits.txt)\n", edits.size());
	}
	Check(UsualCsvBits(tables), "stats 0-15 have the usual CSvBits in this mod's table");
	std::vector<uint8_t> rows(tables.stats.size() * kStatRow, 0);
	for (size_t i = 0; i < tables.stats.size(); ++i) {
		rows[i * kStatRow + kRowCsvBits] = tables.stats[i].csvBits;
	}
	Tables fromRows;
	ShapesFromRows(fromRows, rows.data(), tables.stats.size());
	Check(UsualCsvBits(fromRows), "CSvBits taken from row byte +0xa");

	// The stash's record tail (64 stale bytes after the type-2 record: the game counts its header
	// twice): kept as it is, whatever it holds; a record of an unknown shape is refused.
	const fs::path saves = given;
	{
		Saves real;
		real.load(given, tables);
		DiskIo io0;
		io0.folder = work;
		for (const FixInput& file : real.files) {
			if (!file.stash) {
				continue;
			}
			std::string why, note;
			const bool  safe = StashRecordTailSafe(file.bytes, why, note);
			Check(safe && note.find("kept as is") != std::string::npos, file.name + ": " + note + (safe ? "" : " - " + why));
			size_t record = 0;
			for (size_t at = 0; at + 68 <= file.bytes.size();) {
				uint32_t length, type;
				memcpy(&length, file.bytes.data() + at + 16, 4);
				memcpy(&type, file.bytes.data() + at + 20, 4);
				record = type == 2 ? at : record;
				at += length;
			}
			FixInput junk = file;
			for (size_t i = junk.bytes.size() - 64; i < junk.bytes.size(); ++i) {
				junk.bytes[i] = static_cast<char>(0xa5 ^ i);
			}
			std::string w, n;
			const bool  junkOk = record != 0 && StashRecordTailSafe(junk.bytes, w, n);
			const FixOut junkFix = Fix({ junk }, { { 74, -1 } }, {}, true, io0);
			Check(junkOk && junkFix.ok, "junk bytes in its last 64 bytes: kept as is, the dry run works (" + n + ")");
			FixInput odd = file;
			odd.bytes[record + 64] = 0x11;
			std::string w2, n2;
			const bool  oddOk = StashRecordTailSafe(odd.bytes, w2, n2);
			Check(!oddOk && !w2.empty(), "a record of an unknown shape is refused: " + w2);
		}

		// lf with a count other than 0: the file is unreadable, so refused
		for (FixInput& file : real.files) {
			if (!file.stash) {
				FixInput planted = file;
				planted.bytes[planted.bytes.size() - 2] = 1;
				const uint32_t sum = CharacterChecksum(planted.bytes);
				memcpy(planted.bytes.data() + 12, &sum, 4);
				const FixOut lf = Fix({ planted }, { { 74, -1 } }, {}, true, io0);
				Check(!lf.ok && lf.unreadable == 1 && lf.why.find("lf") != std::string::npos, "a character whose lf count is 1 is refused: " + lf.why);
				break;
			}
		}
	}

	Saves all;
	all.load(saves, tables);
	Check(!all.files.empty(), std::to_string(all.files.size()) + " save files (copies)");

	// which stats the items hold, the attributes
	std::map<uint16_t, int> holders;
	int                     attributesRead = 0, characters = 0;
	std::set<uint16_t>      attributeStats;
	for (const FixInput& file : all.files) {
		const FileResult read = ReadAs(file, tables);
		if (!file.stash) {
			++characters;
			attributesRead += read.attributesRead ? 1 : 0;
			for (const StatValue& a : read.attributes) {
				attributeStats.insert(a.stat);
			}
		}
		for (const Item& item : read.items) {
			for (const StatValue& s : item.stats) {
				++holders[s.stat];
			}
		}
	}
	std::string attrs;
	for (uint16_t s : attributeStats) {
		attrs += " " + std::to_string(s);
	}
	Check(characters > 0 && attributesRead == characters, "character attributes (gf) read in " + std::to_string(attributesRead) + " of " + std::to_string(characters) + " characters; stats:" + attrs);

	// the stat to remove: held most often, 60 or more, not saved with another
	uint16_t removed = 0;
	for (const auto& [stat, count] : holders) {
		if (stat >= 60 && !InGroup(stat) && (removed == 0 || count > holders[removed])) {
			removed = stat;
		}
	}
	int above = 0;
	for (const auto& [stat, count] : holders) {
		above += stat > removed ? count : 0;
	}
	const uint16_t count = static_cast<uint16_t>(tables.stats.size());
	std::vector<ChangeSpec> middle { { removed, -1 } };
	for (uint16_t s = static_cast<uint16_t>(removed + 1); s < count; ++s) {
		middle.push_back({ s, s - 1 });
	}
	printf("      middle row removed: stat %u (%d entries), %d entries above it move one down\n", removed, holders[removed], above);

	// 1. dry run: nothing written
	Fresh(saves, work);
	DiskIo io;
	io.folder = work;
	{
		const FixOut out = Fix(all.files, middle, {}, true, io);
		Check(out.ok && out.dropped == static_cast<uint32_t>(holders[removed]) && out.renumbered == static_cast<uint32_t>(above) && io.writes == 0 && io.backups == 0 && SameOnDisk(saves, work),
			"dry run: would change " + Counts(out) + "; nothing written" + (out.ok ? "" : " - " + out.why));
	}
	// 2. the removal, for real
	{
		const FixOut out = Fix(all.files, middle, {}, false, io);
		Check(out.ok && io.backups == 1 && out.dropped == static_cast<uint32_t>(holders[removed]) && out.renumbered == static_cast<uint32_t>(above), "removal: " + Counts(out) + ", backup made" + (out.ok ? "" : " - " + out.why));
		Check(SameOnDisk(saves, work / "backup-1"), "the backup holds every file as it was");
		StatMap map;
		std::string why;
		MakeMap(tables, middle, {}, map, why);
		bool allRead = true, gone = true, shorter = true;
		size_t before = 0, after = 0;
		for (const FixInput& file : all.files) {
			FixInput now = file;
			now.bytes    = Slurp(work / file.name);
			const FileResult read = ReadAs(now, map.after);
			allRead = allRead && read.ok;
			for (const Item& item : read.items) {
				for (const StatValue& s : item.stats) {
					gone = gone && s.stat != count - 1; // the last number no longer exists
				}
			}
			before += file.bytes.size();
			after += now.bytes.size();
			shorter = shorter && now.bytes.size() <= file.bytes.size();
			if (!read.ok) {
				printf("      %s: %s\n", file.name.c_str(), read.problem.c_str());
			}
		}
		Check(allRead && gone && shorter && after < before, "every rewritten file reads to its end with the table after the removal (" + std::to_string(before) + " -> " + std::to_string(after) + " bytes in all)");
		Saves again;
		again.load(work, map.after);
		bool alreadyFine = true;
		for (const FixInput& file : again.files) {
			alreadyFine = alreadyFine && ReadAs(file, map.after).ok;
		}
		const FixOut twice = Fix(again.files, {}, {}, true, io);
		Check(alreadyFine && twice.ok && twice.changed == 0, "a fix with no change on the rewritten files changes nothing");
	}
	// 3. renumber round trip: a stat nobody holds dropped (the ones above move down), then put back: the very same bytes
	{
		uint16_t free = 0;
		for (uint16_t s = 60; s < count && free == 0; ++s) {
			if (holders.count(s) == 0 && !InGroup(s) && tables.stats[s].csvBits == 0) {
				int heldAbove = 0;
				for (const auto& [stat, n] : holders) {
					heldAbove += stat > s ? n : 0;
				}
				free = heldAbove > 0 ? s : 0;
			}
		}
		std::vector<ChangeSpec> down { { free, -1 } }, up;
		for (uint16_t s = static_cast<uint16_t>(free + 1); s < count; ++s) {
			down.push_back({ s, s - 1 });
			up.push_back({ static_cast<uint16_t>(s - 1), s });
		}
		Fresh(saves, work);
		DiskIo io2;
		io2.folder = work;
		const FixOut a = Fix(all.files, down, {}, false, io2);
		StatMap map;
		std::string why;
		MakeMap(tables, down, {}, map, why);
		Saves moved;
		moved.load(work, map.after);
		std::vector<ChangeSpec> upChanges;
		for (const ChangeSpec& c : up) {
			upChanges.push_back(c);
		}
		const FixOut b = Fix(moved.files, upChanges, {}, false, io2);
		Check(a.ok && b.ok && a.renumbered > 0 && a.renumbered == b.renumbered && SameOnDisk(saves, work),
			"renumber round trip: stat " + std::to_string(free) + " (held by none) dropped, " + std::to_string(a.renumbered) + " entries moved down and back up: every file byte for byte as before" + (a.ok && b.ok ? "" : " - " + a.why + b.why));
	}
	// 4. resize round trip, and a refused resize
	{
		uint16_t held = 0;
		for (const auto& [stat, n] : holders) {
			if (stat >= 60 && !InGroup(stat) && tables.stats[stat].saveBits <= 20 && (held == 0 || n > holders[held])) {
				held = stat;
			}
		}
		const StatShape old = tables.stats[held];
		Fresh(saves, work);
		DiskIo io3;
		io3.folder = work;
		const ResizeSpec bigger { held, static_cast<uint8_t>(old.saveBits + 3), old.paramBits, old.saveAdd + 5 };
		const FixOut     a = Fix(all.files, {}, { bigger }, false, io3);
		StatMap map;
		std::string why;
		MakeMap(tables, {}, { bigger }, map, why);
		bool same = true;
		for (const FixInput& file : all.files) {
			FixInput now = file;
			now.bytes    = Slurp(work / file.name);
			const FileResult x = ReadAs(file, tables), y = ReadAs(now, map.after);
			same = same && y.ok && x.items.size() == y.items.size();
			for (size_t i = 0; same && i < x.items.size(); ++i) {
				same = x.items[i].stats.size() == y.items[i].stats.size();
				for (size_t s = 0; same && s < x.items[i].stats.size(); ++s) {
					same = x.items[i].stats[s].stat == y.items[i].stats[s].stat && x.items[i].stats[s].value == y.items[i].stats[s].value;
				}
			}
		}
		Saves wide;
		wide.load(work, map.after);
		const ResizeSpec back { held, old.saveBits, old.paramBits, old.saveAdd };
		const FixOut     b = Fix(wide.files, {}, { back }, false, io3);
		Check(a.ok && a.resized == static_cast<uint32_t>(holders[held]) && same && b.ok && SameOnDisk(saves, work),
			"resize round trip: stat " + std::to_string(held) + " save bits " + std::to_string(old.saveBits) + " -> " + std::to_string(bigger.saveBits) + ", save add " + std::to_string(old.saveAdd) + " -> "
			    + std::to_string(bigger.saveAdd) + ": " + std::to_string(a.resized) + " values re-encoded, same values read back, and back again = every file byte for byte as before");

		// too small: refused, nothing written, no backup
		int maxRaw = 0;
		for (const FixInput& file : all.files) {
			for (const Item& item : ReadAs(file, tables).items) {
				for (const StatValue& s : item.stats) {
					maxRaw = s.stat == held ? std::max(maxRaw, s.value + old.saveAdd) : maxRaw;
				}
			}
		}
		uint8_t small = 1;
		while ((1 << small) <= maxRaw) {
			++small;
		}
		--small;
		Fresh(saves, work);
		DiskIo io4;
		io4.folder = work;
		const FixOut c = Fix(all.files, {}, { { held, small, old.paramBits, old.saveAdd } }, false, io4);
		size_t refusedFiles = 0;
		for (const FixFileOut& f : c.files) {
			refusedFiles += f.state == FixState::Refused ? 1 : 0;
		}
		Check(!c.ok && refusedFiles > 0 && io4.writes == 0 && io4.backups == 0 && SameOnDisk(saves, work), "a resize to " + std::to_string(small) + " save bits (largest saved value " + std::to_string(maxRaw) + ") is refused, nothing written: " + c.why);
	}
	// 5. the compare catches a planted difference (in memory, and on disk: then everything is put back)
	{
		StatMap map;
		std::string why;
		MakeMap(tables, middle, {}, map, why);
		bool caught = false, any = false;
		for (const FixInput& file : all.files) {
			const FileResult read = ReadAs(file, tables);
			Rewrite          r;
			if (!RewriteFile(file.stash, file.bytes, read, map, r, why) || !r.changed) {
				continue;
			}
			const FileResult now = ReadAs({ file.name, file.stash, r.bytes, &map.after }, map.after);
			for (const Item& item : now.items) {
				if (item.lists.empty() || item.stats.empty() || any) {
					continue;
				}
				any                 = true;
				std::string planted = r.bytes;
				const size_t bit    = item.lists[0].first + 9 + map.after.stats[item.stats[0].stat].paramBits; // the first bit of the first value
				planted[bit >> 3]   = static_cast<char>(planted[bit >> 3] ^ (1 << (bit & 7)));
				if (!file.stash) {
					const uint32_t sum = CharacterChecksum(planted);
					memcpy(planted.data() + 12, &sum, 4);
				}
				std::string found;
				caught = !CheckRewrite(file.stash, file.bytes, read, planted, map, r, found);
				printf("      planted in %s, %s: %s\n", file.name.c_str(), item.code.c_str(), found.c_str());
			}
		}
		Check(any && caught, "a value bit flipped in a rewritten item is caught by the compare");

		Fresh(saves, work);
		DiskIo io5;
		io5.folder  = work;
		io5.plantAt = 1; // the second file written gets a flipped byte on disk
		const FixOut out = Fix(all.files, middle, {}, false, io5);
		Check(!out.ok && out.restored >= 2 && out.notRestored == 0 && SameOnDisk(saves, work), "a difference planted on disk: " + std::to_string(out.restored) + " written files put back, every file as before: " + out.why);

		Fresh(saves, work);
		DiskIo io6;
		io6.folder     = work;
		io6.failBackup = 1;
		const FixOut nb = Fix(all.files, middle, {}, false, io6);
		Check(!nb.ok && io6.writes == 0 && SameOnDisk(saves, work), "no backup, no writing: " + nb.why);
	}
	// 6. refusals of the change list itself
	{
		DiskIo io7;
		io7.folder = work;
		Fresh(saves, work);
		const FixOut g = Fix(all.files, { { 17, -1 } }, {}, false, io7);
		const FixOut d = Fix(all.files, { { 300, 301 }, { 301, 301 } }, {}, false, io7);
		const FixOut a = Fix(all.files, { { 7, 600 } }, {}, false, io7);
		Check(!g.ok && !d.ok && !a.ok && io7.writes == 0 && SameOnDisk(saves, work), "refused: a group stat (" + g.why + "); two stats on one number (" + d.why + "); a number past 510");
		const FixOut at = Fix(all.files, { { 12, 19 }, { 19, 12 } }, {}, false, io7);
		Check(!at.ok && io7.writes == 0, "a change that moves a character attribute (level 12 <-> 19) is refused: " + at.why);
	}
	// 7. saved affix ids: renumber round trip, a drop, refusals
	{
		std::map<uint32_t, int> carriers; // (suffix << 16 | id) -> items
		for (const FixInput& file : all.files) {
			for (const Item& item : ReadAs(file, tables).items) {
				for (const AffixSlot& slot : item.affixes) {
					carriers[static_cast<uint32_t>(slot.suffix) << 16 | slot.id] += slot.id != 0 ? 1 : 0;
				}
			}
		}
		uint16_t common = 0;
		for (const auto& [key, n] : carriers) {
			if ((key >> 16) == 0 && (key & 0xffff) != 0 && (common == 0 || n > carriers[common])) {
				common = static_cast<uint16_t>(key & 0xffff);
			}
		}
		uint16_t other = 0;
		for (const auto& [key, n] : carriers) {
			other = (key >> 16) == 0 && (key & 0xffff) != 0 && (key & 0xffff) != common && n > 0 && other == 0 ? static_cast<uint16_t>(key & 0xffff) : other;
		}
		Fresh(saves, work);
		DiskIo io8;
		io8.folder = work;
		const std::vector<AffixSpec> swap { { 0, common, other }, { 0, other, common } };
		const FixOut a = Fix(all.files, {}, {}, false, io8, swap);
		Saves moved;
		moved.load(work, tables);
		bool movedOk = true;
		int  seen    = 0;
		for (const FixInput& file : moved.files) {
			const FileResult read = ReadAs(file, tables);
			movedOk = movedOk && read.ok;
			for (const Item& item : read.items) {
				for (const AffixSlot& slot : item.affixes) {
					seen += slot.suffix == 0 && slot.id == other ? 1 : 0;
				}
			}
		}
		const FixOut b = Fix(moved.files, {}, {}, false, io8, swap);
		Check(a.ok && b.ok && movedOk && seen == carriers[common] && a.affixesRenumbered == static_cast<uint32_t>(carriers[common] + carriers[other]) && SameOnDisk(saves, work),
			"prefixes " + std::to_string(common) + " <-> " + std::to_string(other) + " swapped on " + std::to_string(a.affixesRenumbered) + " affixes and back: every file byte for byte as before" + (a.ok && b.ok ? "" : " - " + a.why + b.why));

		// drop: a prefix whose magic carriers all have a suffix too
		uint16_t droppable = 0, lonely = 0;
		std::map<uint16_t, bool> fine;
		for (const FixInput& file : all.files) {
			for (const Item& item : ReadAs(file, tables).items) {
				if (item.quality == 4 && item.affixes.size() == 2 && item.affixes[0].id != 0) {
					const bool ok = item.affixes[1].id != 0;
					fine.emplace(item.affixes[0].id, true);
					fine[item.affixes[0].id] = fine[item.affixes[0].id] && ok;
					lonely = !ok && lonely == 0 ? item.affixes[0].id : lonely;
				}
			}
		}
		for (const auto& [id, ok] : fine) {
			droppable = ok && (droppable == 0 || carriers[id] > carriers[droppable]) ? id : droppable;
		}
		Fresh(saves, work);
		DiskIo io9;
		io9.folder = work;
		const FixOut d = Fix(all.files, {}, {}, false, io9, { { 0, droppable, kDroppedAffix } });
		int  zeroed = 0, rareShorter = 0;
		bool statsKept = true, readOk = true;
		for (const FixInput& file : all.files) {
			FixInput now = file;
			now.bytes    = Slurp(work / file.name);
			const FileResult x = ReadAs(file, tables), y = ReadAs(now, tables);
			readOk = readOk && y.ok && x.items.size() == y.items.size();
			for (size_t i = 0; readOk && i < x.items.size(); ++i) {
				bool had = false;
				for (const AffixSlot& slot : x.items[i].affixes) {
					had = had || (slot.suffix == 0 && slot.id == droppable);
				}
				if (!had) {
					continue;
				}
				statsKept = statsKept && x.items[i].stats.size() == y.items[i].stats.size();
				for (size_t s = 0; statsKept && s < x.items[i].stats.size(); ++s) {
					statsKept = x.items[i].stats[s].stat == y.items[i].stats[s].stat && x.items[i].stats[s].value == y.items[i].stats[s].value;
				}
				if (x.items[i].quality == 4) {
					zeroed += y.items[i].affixes[0].id == 0 ? 1 : 0;
				} else {
					rareShorter += y.items[i].affixes.size() + 1 == x.items[i].affixes.size() ? 1 : 0;
				}
			}
		}
		Check(d.ok && readOk && statsKept && static_cast<uint32_t>(zeroed + rareShorter) == d.affixesDropped && d.affixesDropped == static_cast<uint32_t>(carriers[droppable]),
			"prefix " + std::to_string(droppable) + " dropped: " + std::to_string(zeroed) + " magic items now have prefix 0 (none), " + std::to_string(rareShorter) + " rare/crafted items lost that slot; every item keeps its stats, every file reads"
			    + (d.ok ? "" : " - " + d.why));
		Fresh(saves, work);
		DiskIo io10;
		io10.folder = work;
		const FixOut e = lonely != 0 ? Fix(all.files, {}, {}, false, io10, { { 0, lonely, kDroppedAffix } }) : FixOut {};
		Check(lonely != 0 && !e.ok && io10.writes == 0 && e.why.find("neither prefix nor suffix") != std::string::npos, "dropping prefix " + std::to_string(lonely) + " from a magic item without a suffix is refused: " + e.why);
		const FixOut c = Fix(all.files, {}, {}, true, io10, { { 0, common, other } });
		const FixOut z = Fix(all.files, {}, {}, true, io10, { { 0, common, 0 } });
		Check(!c.ok && !z.ok, "refused: a prefix onto a number another keeps (" + c.why + "); a prefix onto 0");
	}
	// 8. restore, the game check before writing, made stats the table lacks
	{
		if (!grownRows.empty()) {
			std::vector<MadeRow> missing;
			Saves                real;
			real.load(given, textOnly);
			const auto holding = FilesHoldingMissing(real.files, textOnly, grownRows, missing);
			std::string names;
			for (const std::string& n : holding) {
				names += " " + n;
			}
			printf("      without Live Tables' rows: %zu made rows missing, saves holding them:%s\n", missing.size(), names.empty() ? " none" : names.c_str());
			size_t pastTable = 0;
			for (const FixInput& file : real.files) {
				const FileResult r = ReadAs(file, textOnly);
				pastTable += !r.ok && r.problem.find("past the stat table") != std::string::npos ? 1 : 0;
			}
			Check(missing.size() == grownRows.size() && holding.size() == pastTable && (pastTable == 0 || !holding.empty()),
				"made rows missing from the table (Live Tables off): the saves holding them are named (" + std::to_string(holding.size()) + ":" + names + ")");
			std::vector<MadeRow> none;
			Check(FilesHoldingMissing(real.files, tables, grownRows, none).empty() && none.empty(), "with Live Tables' rows in the table: no warning");
		}
		Fresh(saves, work);
		DiskIo io11;
		io11.folder = work;
		const FixOut f = Fix(all.files, middle, {}, false, io11);
		const bool   planOk = f.ok && io11.plan.size() == f.changed;
		const FixOut r1 = Restore(io11.plan, io11);
		const bool   back1 = SameOnDisk(saves, work);
		const FixOut r2 = Restore(io11.plan, io11);
		Check(planOk && r1.ok && r1.restored == f.changed && back1 && r2.ok && r2.restored == 0 && SameOnDisk(saves, work),
			"restore after a whole fix: " + std::to_string(r1.restored) + " files put back, every file as before; a second restore finds them back already (" + r2.why + ")");

		Fresh(saves, work);
		const FixOut g = Fix(all.files, middle, {}, false, io11);
		// half put back by hand (as if a restore stopped), one file changed since (as if the game saved it)
		const std::string changedName = io11.plan.back().name;
		for (size_t i = 0; i < io11.plan.size() / 2; ++i) {
			std::ofstream(work / io11.plan[i].name, std::ios::binary) << io11.plan[i].backup;
		}
		std::string later = Slurp(work / changedName);
		later[later.size() / 2] = static_cast<char>(later[later.size() / 2] ^ 1);
		std::ofstream(work / changedName, std::ios::binary) << later;
		const FixOut r3 = Restore(io11.plan, io11);
		Check(g.ok && !r3.ok && r3.notRestored == 1 && Slurp(work / changedName) == later && r3.why.find(changedName) != std::string::npos,
			"restore over a mix: " + std::to_string(r3.restored) + " put back, the half already back left, the file changed since NOT overwritten: " + r3.why);

		Fresh(saves, work);
		DiskIo io12;
		io12.folder      = work;
		io12.allowWrites = 3; // the check before the backup + two writes, then "a game started"
		const FixOut h = Fix(all.files, middle, {}, false, io12);
		Check(!h.ok && h.restored == 2 && io12.writes >= 2 && SameOnDisk(saves, work), "a game started during the writing: stopped, the " + std::to_string(h.restored) + " files written put back: " + h.why);
		DiskIo io13;
		io13.folder      = work;
		io13.allowWrites = 0;
		const FixOut i0 = Fix(all.files, middle, {}, false, io13);
		Check(!i0.ok && io13.writes == 0 && io13.backups == 0 && SameOnDisk(saves, work), "in a game before the backup: nothing done: " + i0.why);

	}
	// 9. a made stat that came back at another number (before shapes + the move), and notAfter
	{
		uint16_t held = 0;
		for (const auto& [stat, n] : holders) {
			held = stat >= 60 && !InGroup(stat) && (held == 0 || n > holders[held]) ? stat : held;
		}
		const StatShape made = tables.stats[held];
		Tables          now  = tables; // the table after a "mod update": a new mod stat at `held`, the made one at the end
		now.stats[held].saveBits = static_cast<uint8_t>(made.saveBits + 5);
		const uint16_t at = static_cast<uint16_t>(now.stats.size());
		now.stats.push_back(made);
		Saves current;
		current.load(saves, now);
		int unreadable = 0;
		for (const FixInput& file : current.files) {
			unreadable += ReadAs(file, now).ok ? 0 : 1;
		}
		Tables      reading = now;
		std::string why;
		const bool  applied = ApplyBefore(reading, { { held, made.saveBits, made.paramBits, made.saveAdd } }, why);
		Saves       old;
		old.load(saves, reading);
		Fresh(saves, work);
		DiskIo io15;
		io15.folder = work;
		const FixOut f = Fix(old.files, { { held, at }, { at, -1 } }, {}, false, io15);
		bool same = f.ok;
		int  moved = 0;
		for (const FixInput& file : all.files) {
			FixInput after = file;
			after.bytes    = Slurp(work / file.name);
			const FileResult x = ReadAs(file, tables), y = ReadAs(after, now);
			same = same && y.ok && x.items.size() == y.items.size();
			for (size_t i = 0; same && i < x.items.size(); ++i) {
				same = x.items[i].stats.size() == y.items[i].stats.size();
				for (size_t s = 0; same && s < x.items[i].stats.size(); ++s) {
					const bool was = x.items[i].stats[s].stat == held;
					moved += was ? 1 : 0;
					same = (was ? y.items[i].stats[s].stat == at : y.items[i].stats[s].stat == x.items[i].stats[s].stat) && x.items[i].stats[s].value == y.items[i].stats[s].value;
				}
			}
		}
		Check(applied && unreadable > 0 && same && moved == holders[held],
			"made stat " + std::to_string(held) + " back at " + std::to_string(at) + " after a mod update: " + std::to_string(unreadable) + " files unreadable with the new table; read with the before shape and moved, " + std::to_string(moved)
			    + " entries now read right with the new table" + (f.ok ? "" : " - " + f.why));
		std::vector<FixInput> timed = all.files;
		for (size_t i = 0; i < timed.size(); ++i) {
			timed[i].writeTime = i == 0 ? 2000 : 1000;
		}
		DiskIo io16;
		io16.folder = work;
		const FixOut late = Fix(timed, middle, {}, true, io16, {}, 1500);
		const FixOut fine = Fix(timed, middle, {}, true, io16, {}, 2500);
		Check(!late.ok && late.why.find(timed[0].name) != std::string::npos && fine.ok, "notAfter: a file saved after it is refused by name (" + late.why.substr(0, 120) + "...); all older: fine");
	}
	fs::remove_all(work);
	printf(g_failed == 0 ? "ALL PASSED\n" : "FAILURES: %d\n", g_failed);
	return g_failed == 0 ? 0 : 1;
}
