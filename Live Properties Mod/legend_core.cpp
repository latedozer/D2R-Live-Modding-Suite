// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
#include "legend_core.h"

namespace legend {
namespace {

struct Words {
	const char* code;
	const char* meaning;
	const char* detail;
	const char* insert;
};

void Add(std::vector<Item>& to, const Words& w, const Examples& examples) {
	Item item { w.code, w.meaning, w.detail != nullptr && w.detail[0] != '\0' ? w.detail : w.meaning, w.insert != nullptr ? w.insert : "" };
	if (const auto example = examples.find(w.code); example != examples.end() && !example->second.empty()) {
		item.detail += "\nIn your tables: " + example->second;
	}
	to.push_back(std::move(item));
}

// The data guide's Properties.txt func table (Properties.js), shortened.
struct Func {
	int         func;
	const char* meaning;
	const char* detail;
};
constexpr Func kFuncs[] = {
	{ 0, "nothing (an empty func cell)", "The line does nothing." },
	{ 1, "random min-max (Superior items: max)", "The stat gets a random value from the mod's min to its max. On a Superior (high quality) item it gets the max. Uses stat and set." },
	{ 2, "always the max", "The stat always gets the mod's max. Uses stat and set." },
	{ 3, "like 1, the same roll as the line above", "Same as func 1, but a func-3 line right after another line takes the value worked out for that line: one roll for several stats (res-all)." },
	{ 4, "like 2, the same value as the line above", "Same as func 2, but takes the value worked out for the line before it." },
	{ 5, "the item's minimum damage", "Sets the item's minimum damage. Uses set." },
	{ 6, "the item's maximum damage", "Sets the item's maximum damage (it depends on the minimum). Uses set." },
	{ 7, "enhanced damage %, random min-max", "The damage percent from the mod's min-max. If the item is a weapon whose damage the percent does not change, it gets +1 maximum damage instead." },
	{ 8, "speed stats: random min-max, shared roll", "Random from min to max; lines after it with func 8 take the same value (attack, cast, hit recovery speed)." },
	{ 9, "one skill's level (shared roll)", "Levels of a single skill; needs the stat's min and max. Lines after it take the same value." },
	{ 10, "skill tab levels (shared roll)", "Levels to the skills of a skill tab. Lines after it take the same value." },
	{ 11, "cast a skill on an event", "Item event: the param is the skill, min the percent chance to cast it, max its level." },
	{ 12, "a random param from min-max", "The mod's min and max are the range the stat's param is picked from." },
	{ 13, "max durability: random min-max", "Random from min to max (Superior: the max); also fills the current durability." },
	{ 14, "sockets: min-max, or the param", "The number of sockets: random from min to max when they are given, else the param." },
	{ 15, "always the min", "The stat always gets the mod's min (minimum damage: the item's own)." },
	{ 16, "always the max", "The stat always gets the mod's max (maximum damage: the item's own)." },
	{ 17, "the param (else random min-max, else 0)", "Uses the mod's param as the value; without one a random min-max; else 0." },
	{ 18, "by the time of day", "The value follows the game's time of day: the closer to the stat's own time, the higher." },
	{ 19, "a charged skill", "The param is the skill; min gives the charges, max the skill level." },
	{ 20, "indestructible", "Adds the indestructible stat." },
	{ 21, "random min-max; this line's val is the param", "Random from min to max; the property line's own val column is the stat's param (e.g. which class's skills)." },
	{ 22, "random min-max; the mod's param is the param", "Random from min to max; the mod's param is the stat's param (e.g. which skill)." },
	{ 23, "ethereal (only items with durability)", "Makes the item ethereal, if it has durability." },
	{ 24, "random min-max with the param, shared roll", "Random from min to max with the mod's param as the stat's param; lines after it take the same value." },
	{ 25, "a random stat of another property", "The param names a property; one of its stats is picked, with a value from this mod's min-max." },
	{ 36, "21 swapped: val is the value, min-max the param", "The property line's val is the stat's value; the mod's roll is used as the param." },
};

const Func* FindFunc(int func) {
	for (const Func& f : kFuncs) {
		if (f.func == func) {
			return &f;
		}
	}
	return nullptr;
}

} // namespace

const char* FuncMeaning(int func) {
	const Func* f = FindFunc(func);
	return f != nullptr ? f->meaning : "not in the game's data guide";
}

Section Properties(int funcs, bool (*works)(int), const Examples& examples) {
	Section s;
	const Words how[] = {
		{ "a property", "a code (res-all, dmg%) with up to 7 lines",
			"Items, affixes, cube recipes and runewords name a property by its code, with a min, a max and a param. The game goes through the property's 7 lines in order: each line's func says what it does "
			"with that min, max and param, to the line's stat.",
			nullptr },
		{ "a line", "func, stat, set, val: what one line of a property does",
			"func: what it does (the list below). stat: the stat it works on (a name from the Stats tab). set: 1 = the value replaces the stat's, 0 = it is added. val: a number some funcs use (func 21: the param).",
			nullptr },
		{ "saving", "a saved change counts for items made from then on",
			"An item keeps the stats it got when it was made: the property itself is not saved on items. Every data version with the same code gets the change. Kept in property-edits.txt and put back each time "
			"the game reads its tables; Restore gives the game's own row back.",
			nullptr },
		{ "shared rolls", "funcs 3 4 8 9 10 24 reuse the line above's roll",
			"One roll for several stats: a line with one of these funcs takes the value worked out for the line before it, so all four resistances of res-all come out the same.", nullptr },
		{ "new property", "a code of your own, added at once (Live Tables)",
			"New property asks the Live Tables plugin for a row under the code you type: it is added to the property table at once, during play, in every data version, and Live Tables "
			"keeps it for the next starts. Then fill its lines on its card; recipes, affixes, uniques and runewords can name it. Kept in made-properties.txt; its lines in property-edits.txt. "
			"Without Live Tables the button does nothing.",
			nullptr },
		{ "disable", "Disable: gives nothing; rows are never removed",
			"Disable: its 7 funcs do nothing in memory, the row stays, so everything that names it still loads (property-off.txt); Enable gives it back. Restore of a made one: "
			"its lines are cleared at once (it gives nothing); the row stays. Disable asks first and lists what names the property. Items keep the stats they have.",
			nullptr },
		{ "filling a cell", "click a Funcs or Stats row, or Insert here",
			"With a func or stat cell of the property card open, a click on a row of the Funcs or the Stats tab puts it in; this Legend's Insert does the same.", nullptr },
	};
	for (const Words& w : how) {
		Add(s.how, w, examples);
	}
	const Words columns[] = {
		{ "func", "what the line does (a number from the list below)", "The property func: a number of the list below. Empty or 0: the line does nothing; a number the game has no handler for is refused.", nullptr },
		{ "stat", "the stat the line works on", "A stat name from the Stats tab (maxhp, fireresist) or its number. Some funcs need none (sockets, ethereal).", nullptr },
		{ "set", "1: the value replaces the stat's; 0: it is added", "Boolean (data guide): 1 sets the stat to the value whatever it was; 0 adds to it.", nullptr },
		{ "val", "a fixed number some funcs use", "An integer the func may use: func 21 takes it as the stat's param, func 36 as the value.", nullptr },
	};
	for (const Words& w : columns) {
		Add(s.list, w, examples);
	}
	for (int func = 0; func < funcs; ++func) {
		const std::string code = "func " + std::to_string(func);
		const Func*       f    = FindFunc(func);
		Item              item;
		item.code   = code;
		item.insert = std::to_string(func);
		if (func != 0 && (works == nullptr || !works(func))) {
			item.meaning = "does nothing: the game has no handler for it";
			item.detail  = "This game has no handler for func " + std::to_string(func) + ": a line with it would be skipped, so the plugin refuses it.";
			item.insert.clear();
		} else if (f == nullptr) {
			item.meaning = "the game has a handler; the data guide has no words";
			item.detail  = "The game has a handler for func " + std::to_string(func) + ", but its data guide does not say what it does (not looked into).";
		} else {
			item.meaning = f->meaning;
			item.detail  = f->detail;
		}
		if (const auto example = examples.find(code); example != examples.end() && !example->second.empty()) {
			item.detail += "\nIn your tables: " + example->second;
		}
		s.list.push_back(std::move(item));
	}
	return s;
}

Section Stats(const Examples& examples) {
	Section s;
	const Words how[] = {
		{ "kept as", "x256: the game keeps it 256 times the number shown",
			"val shift 8: life, mana and stamina are kept in 256ths (a list row says \"kept x256\"). An op works on the kept number, so an op from a x256 stat to a plain one gives 256 times too much, "
			"and from plain to x256 256 times too little.",
			nullptr },
		{ "room", "a stat takes 16 op additions; room = how many are free",
			"Every stat that names this one as its op stat 1-3 takes one of its 16 places (the game makes these lists when it reads its tables). An edit that would need a 17th is refused.", nullptr },
		{ "dep", "a stat can be the op base of 64 stats",
			"Every op stat whose op base is this stat is worked out from it (a list row: \"N stats are worked out from it\"). 64 at most.", nullptr },
		{ "saved", "its 5 save cells are refused while items hold it",
			"These cells decide how an item holding the stat is read from a save. They are refused while the character in game carries such an item; with Save Scan installed every saved character "
			"and the shared stash are read first. A character holding an item with a made stat cannot be loaded without this plugin.",
			nullptr },
		{ "made stats", "New: a stat of yours, added at once (Live Tables)",
			"Stats > New asks the Live Tables plugin for a row: stat<n>, 13 bits, may be negative, no text, no op, added after the table's last row at once, during play, never above 510. "
			"Live Tables keeps it for the next starts. A made stat is kept by its name (stat-edits.txt). Give it a text and an op on its card, and put it on items through a property line "
			"with func 1. Without Live Tables the button does nothing.",
			nullptr },
		{ "rename", "a made stat's name / property's code, at once",
			"Type a new name (stat card) or code (property card) and Save: Live Tables renames the row in the game's name table, its number stays, and this plugin's edit files follow "
			"(dated backups). The game's and the mod's own rows can't be renamed: the files read at load name them by it.",
			nullptr },
		{ "disable/remove", "Disable: no effect; Remove: the row goes",
			"Disable (Live Tables): the stat keeps its number and how it is saved, but has no op, no line on items and no item event; Enable gives it back, everywhere it was. "
			"'Disable all' also disables the properties that name it. Remove (made stats only): Live Tables takes the row out, refused while a property or another stat still names it. "
			"The last row can go in a game; a middle row only at the main menu (every later stat one lower). Before the question Save Scan shows which saves would be fixed; "
			"on Yes they are fixed first (dated backup, write, read back, compare, everything put back on any difference). stat-edits.txt follows the new numbers (dated backup).",
			nullptr },
		{ "limits", "stats 0-510 on items; an op result below 2,147,483,647",
			"Saved items hold stat numbers in 9 bits (0-510). An op multiplies two kept numbers: with x256 stats keep the shown numbers small (RegenCharm met a wrap-around above about 8 million).", nullptr },
	};
	for (const Words& w : how) {
		Add(s.how, w, examples);
	}
	const Words columns[] = {
		{ "signed", "1: the number may be negative", "Data guide: 1 = a signed number (positive or negative), 0 = only positive. How items are saved.", nullptr },
		{ "send bits", "bits the number takes when sent to the client", "Data guide: how many bits are sent to the game client, which caps the value. Signed values should take fewer than 32.", nullptr },
		{ "save bits", "bits on a saved item (0: not saved)", "How many bits a saved item gives the number: 9 bits hold 0-511 (-256..255 when signed). 0 = not saved on items.", nullptr },
		{ "save add", "added before saving, so negatives fit", "Added to the number before it is saved and taken off when read: with save add 32 the item can hold -32 and up.", nullptr },
		{ "val shift", "8: kept 256 times larger than shown", "Data guide: shifts the stat's value by this many bits for calculations. 8 = kept x256 (life, mana, stamina). Changes what every number of it means.", nullptr },
		{ "desc priority", "the line's place on an item (higher = higher up)", "Data guide: tooltips are sorted by this; the same priority keeps the table's order.", nullptr },
		{ "desc func", "how the line is written (the descfunc list below)", "A number of the descfunc list below; 0 = no line on the item.", nullptr },
		{ "desc val", "0 no number, 1 number first, 2 number last", "Data guide: where the value stands in the line.", nullptr },
		{ "desc str pos", "the line's text for a positive value", "A string key (ModStr1a) or the number of a string a stat already uses.", nullptr },
		{ "desc str neg", "the line's text for a negative value", "The same, for a negative value.", nullptr },
		{ "op", "what the stat does to other stats (the op list below)", "A number of the op list below. Empty = nothing.", nullptr },
		{ "op param", "the op's divisor: 2 to this power", "The op's result is divided by 2 to this power (12 = 1/4096).", nullptr },
		{ "op base", "the stat the op multiplies by (level, maxhp)", "A stat name from the list, or its number.", nullptr },
		{ "op stat 1", "the stat the op's result goes to", "The first stat the result is added to.", nullptr },
		{ "op stat 2", "a second stat it goes to", "Read only when op stat 1 is filled.", nullptr },
		{ "op stat 3", "a third one", "Read only when op stat 2 is filled.", nullptr },
	};
	for (const Words& w : columns) {
		Add(s.list, w, examples);
	}
	// The data guide's op table. It writes "<< op param"; the game divides (>>), as RegenCharm found.
	const Words ops[] = {
		{ "op 0", "no op: the stat just counts as itself", nullptr, "0" },
		{ "op 1", "the op stat grows by this value in %", "Percent: op stat += op stat x value / 100.", "1" },
		{ "op 2", "adds value x op base, divided by 2^op param", "By level: op stat += value x op base >> op param. This stat's value times the op base (e.g. level), divided by 2 to the power op param, is added to the op stat. The data guide writes <<; the game divides.", "2" },
		{ "op 3", "like op 2, but as a % of the op stat", "By level percent: value x op base >> op param is a percent the op stat grows by.", "3" },
		{ "op 4", "like op 2, the op base taken from the item", "By level source: as op 2, but the op base's value is the item's, not the unit's.", "4" },
		{ "op 5", "like op 3, the op base taken from the item", "By level source percent: as op 3, with the item's value.", "5" },
		{ "op 6", "follows the time of day", "By time: grows the op stat by a delta from the stat's min-max that follows the game's time of day.", "6" },
		{ "op 7", "follows the time of day, as a %", "By time percent: as op 6, as a percent.", "7" },
		{ "op 8", "energy gives mana (charstats ManaPerMagic)", "Players only: op stat = value x ManaPerMagic, in mana's 256ths.", "8" },
		{ "op 9", "vitality gives life and stamina (charstats)", "Players only: LifePerVitality (or StaminaPerVitality for maxstamina), in 256ths.", "9" },
		{ "op 10", "not used: does nothing", nullptr, "10" },
		{ "op 11", "the op stat grows by this value in %, on units", "Player percent: as op 1, only for stats on units (item_maxhp_percent).", "11" },
		{ "op 12", "no words in the data guide", nullptr, "12" },
		{ "op 13", "the op stat grows by this value in %, on items", "Item percent: as op 1, only for stats on items (enhanced defense to defense).", "13" },
	};
	for (const Words& w : ops) {
		Add(s.list, w, examples);
	}
	const Words descs[] = {
		{ "descfunc 0", "no line", nullptr, "0" },
		{ "descfunc 1", "+N text (-N when negative)", nullptr, "1" },
		{ "descfunc 2", "N% text", nullptr, "2" },
		{ "descfunc 3", "N text", nullptr, "3" },
		{ "descfunc 4", "+N% text", nullptr, "4" },
		{ "descfunc 5", "+(N x 100 / 128)% text", nullptr, "5" },
		{ "descfunc 6", "+N text text2 (per level)", nullptr, "6" },
		{ "descfunc 7", "N% text text2 (per level)", nullptr, "7" },
		{ "descfunc 8", "+N% text text2 (per level)", nullptr, "8" },
		{ "descfunc 9", "N text text2 (per level)", nullptr, "9" },
		{ "descfunc 10", "(N x 100 / 128)% text text2 (per level)", nullptr, "10" },
		{ "descfunc 11", "repair (string ModStre9t)", nullptr, "11" },
		{ "descfunc 12", "+N text when N > 1, else N text", nullptr, "12" },
		{ "descfunc 13", "+N to class skills (charstats StrAllSkills)", nullptr, "13" },
		{ "descfunc 14", "+N to a skill tab (charstats StrSkillTab)", nullptr, "14" },
		{ "descfunc 15", "chance to cast: skill, level, percent", nullptr, "15" },
		{ "descfunc 16", "aura: skill name, level", nullptr, "16" },
		{ "descfunc 17", "by time of day", nullptr, "17" },
		{ "descfunc 18", "by time of day (as 17)", nullptr, "18" },
		{ "descfunc 19", "the text with N put in (sprintf), + text2", nullptr, "19" },
		{ "descfunc 20", "-N% text", nullptr, "20" },
		{ "descfunc 21", "-N% text text2 (per level)", nullptr, "21" },
		{ "descfunc 22", "N% versus a monster type", nullptr, "22" },
		{ "descfunc 23", "reanimate as a monster", nullptr, "23" },
		{ "descfunc 24", "charges: skill, level, charges", nullptr, "24" },
		{ "descfunc 25", "minus: +(-N) text, or text +(-N)", nullptr, "25" },
		{ "descfunc 26", "minus per level (as 25)", nullptr, "26" },
		{ "descfunc 27", "+N to one skill (class only)", nullptr, "27" },
		{ "descfunc 28", "+N to one skill (any class)", nullptr, "28" },
		{ "descfunc 29", "as 19 with N made positive", nullptr, "29" },
	};
	for (const Words& w : descs) {
		Add(s.list, w, examples);
	}
	return s;
}

Section Affixes(const Examples& examples) {
	Section s;
	const Words how[] = {
		{ "one table", "suffixes, prefixes and automagic share one table",
			"The game reads MagicSuffix, MagicPrefix and automagic into one table, in that order. Each kind counts its own rows (prefix 135, suffix 9); New takes an empty row, so numbers never move.", nullptr },
		{ "pre= / suf=", "a recipe forces an affix: cm1,mag,pre=135",
			"A cube recipe's output with mag and pre= (or suf=) asks for that row. The game takes it only if it is spawnable, its frequency is above 0, the item's affix level reaches its level and the item's "
			"type is listed; else it picks a random one. The plugin counts rows from 0 (Prismatic is pre=331); not yet confirmed in game.",
			"pre=" },
		{ "frequency", "its chance: its frequency / the sum of all that fit",
			"Data guide: the frequencies of every affix that can be on the item are summed; one is picked at random by weight.", nullptr },
		{ "level", "the item's affix level must reach it (at most 99)",
			"Drops get their level from the monster; a recipe from its lvl column. An affix of level 99 stays off nearly every drop.", nullptr },
		{ "disable", "spawnable 0, frequency 0: on no drop, no recipe",
			"Disable on the card sets spawnable and frequency to 0 in the game's memory (affix-off.txt): it rolls on no drop and pre= / suf= cannot force it. Items that have it keep it. "
			"Enable gives back the cells it had.", nullptr },
		{ "name", "a string key (Stout) or your own words",
			"The name shown on the item: one of the game's string keys, or your own words (the plugin has 16 strings of 40 letters for them).", nullptr },
	};
	for (const Words& w : how) {
		Add(s.how, w, examples);
	}
	const Words list[] = {
		{ "Cube only", "spawnable 1, frequency 1, level 99", "The affix is kept off drops (no item is level 99 often); give the recipe lvl 99 so its output reaches it.", nullptr },
		{ "Rare drops", "spawnable 1, frequency 1", "The lowest frequency that still lets a recipe force it; it may also drop, seldom.", nullptr },
		{ "Normal drops", "spawnable 1, frequency 4", "Like most of the game's affixes.", nullptr },
		{ "spawnable", "1: may be on items (needed for pre=/suf=)", "Data guide: 1 = the game's randomizer may use it; 0 = never.", nullptr },
		{ "rare", "1: may also be on rare items", "Data guide: 0 = not used for rare items.", nullptr },
		{ "level", "the lowest item level it comes on", "Data guide: below this item level it does not spawn.", nullptr },
		{ "maxlevel", "the highest item level it comes on", "Data guide: above it it does not spawn. Empty: no top.", nullptr },
		{ "levelreq", "the character level needed to use the item", nullptr, nullptr },
		{ "frequency", "how often it is picked (0: never)", "Weighed against all affixes that fit. Most of the game's: 1-4.", nullptr },
		{ "group", "one affix of a group per item", "Data guide: an item never has two affixes of the same group. Empty = group 0.", nullptr },
		{ "classspecific", "only on this class's items", "Data guide: uses the class of the item's type (ItemTypes Class).", nullptr },
		{ "class", "the class of classlevelreq", nullptr, nullptr },
		{ "classlevelreq", "the level that class needs instead", "Data guide: empty = levelreq for every class.", nullptr },
		{ "mod code", "mod1-3: a property code (Properties tab)", "The property each of its 3 mods gives.", nullptr },
		{ "mod param", "the property's param (a skill, a level ...)", "Its use depends on the property's func.", nullptr },
		{ "mod min", "the lowest roll", "Its use depends on the property's func. The game's own rows may have min above max.", nullptr },
		{ "mod max", "the highest roll", "Its use depends on the property's func.", nullptr },
		{ "itype", "itype1-7: item types it may be on (scha, ring)", "An ItemTypes code; a parent type covers its children (char = every charm).", nullptr },
		{ "etype", "etype1-5: item types it is never on", nullptr, nullptr },
	};
	for (const Words& w : list) {
		Add(s.list, w, examples);
	}
	return s;
}

} // namespace legend
