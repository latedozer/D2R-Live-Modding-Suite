// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Large Charm Life Regen (v2)
//
// Adds "+N% of Max Life as Replenish Life" to Large Charms:
//  1. Three magic suffix tiers, each with its own name, %, item level,
//     required level and frequency.
//  2. A unique Large Charm that Gheed can sell (optional).
//
// How the effect works
// --------------------
// A hidden stat uses the ItemStatCost "op 2" formula:
//     hpregen += (value * maxLife) >> param
// The game can only divide by powers of two, so N% is stored as N * 41 and
// divided by 2^12 (41 / 4096 = 1.0009% per point). hpregen is in 1/256 life
// per frame, so 1% of max life as Replenish Life heals about 0.1% of max
// life per second.
//
// ValShift must be 0 on the real and total stats. Item properties are stored
// as value << ValShift, and the rows are cloned from item_hp_perlevel
// (ValShift 8). With ValShift 8 the stat held N * 41 * 256, and the op's
// 32-bit multiply (done by D2RLoader's D2RCore, not D2R.exe, so a JSON patch
// can't fix it) wrapped negative at +2048 Replenish Life: life drain.
//
// Overflow limit now: total N% * maxLife < 52,377,649 (2^31 / 41). At the
// 8,191 max life save cap that is a total of 6,394%. Raising the cap with the
// Max Life/Mana Save Fix mod lowers this (e.g. 65,535 life -> 799%).
//
// Per charm: the stats have 13 signed bits, so N * 41 must stay <= 4095,
// which is why one charm can have at most 99%.
//
// Save compatibility: rows are only ever appended, in a fixed order, and all
// three tier rows are always written (a disabled tier just can't spawn), so
// the stat IDs and suffix IDs of existing charms never move. Tier 1 keeps the
// v1 suffix key, so v1 "of Renewal" charms become tier 1 charms.
//
// Each charm carries these mods:
//   regen-mh      value N*41: hidden real stat + "+X Replenish Life (Based on Max Life)"
//   regen-mh-pct  value N:    "+N% of Max Life as Replenish Life" (display only)
//   regen         value 1:    optional flat +1 Replenish Life ("Round up" setting)

const MAX_PCT = 99;
const SCALE = 41;
const SHIFT = 12;

const STAT_REAL = 'item_hpregen_permaxlife';
const STAT_DISPLAY = 'item_hpregen_permaxlife_display';
const STAT_TOTAL = 'item_hpregen_permaxlife_total';
const STAT_DUMMY = 'item_hpregen_permaxlife_dummy';
const PROP_REAL = 'regen-mh';
const PROP_DISPLAY = 'regen-mh-pct';
const DESC_PCT = 'ModStrLargeCharmRegenMH';
const DESC_TOTAL = 'ModStrLargeCharmRegenMHTotal';
const UNIQUE_KEY = 'LargeCharmLifeRegenUnique';

// Tier 1 keeps the v1 key so existing charms keep their name.
const TIERS = [
    { key: 'LargeCharmLifeRegen', id: 'tier1' },
    { key: 'LargeCharmLifeRegen2', id: 'tier2' },
    { key: 'LargeCharmLifeRegen3', id: 'tier3' },
].map((t) => ({
    ...t,
    enabled: config[t.id] !== false,
    name: config[`${t.id}Name`],
    pct: config[`${t.id}Pct`],
    level: config[`${t.id}Level`],
    levelReq: config[`${t.id}LevelReq`],
    frequency: config[`${t.id}Frequency`],
}));

const LANGS = ['enUS', 'zhTW', 'deDE', 'esES', 'frFR', 'itIT', 'koKR', 'plPL', 'esMX', 'jaJP', 'ptBR', 'ruRU', 'zhCN'];

const clampPct = (n) => Math.max(1, Math.min(MAX_PCT, Math.round(Number(n) || 1)));
const norm = (h) => h.replace(/\s+/g, '').toLowerCase();
const findCol = (headers, name) => headers.find((h) => norm(h) === norm(name));

function addString(path, key, text) {
    const strings = D2RMM.readJson(path);
    const entry = { id: D2RMM.getNextStringID(), Key: key };
    LANGS.forEach((lang) => {
        entry[lang] = text;
    });
    strings.push(entry);
    D2RMM.writeJson(path, strings);
}

function blankRow(headers) {
    const row = {};
    headers.forEach((h) => {
        row[h] = '';
    });
    return row;
}

// The mods that give N%.
function regenMods(pct) {
    const n = clampPct(pct);
    return [
        { code: PROP_REAL, min: n * SCALE, max: n * SCALE },
        { code: PROP_DISPLAY, min: n, max: n },
        // Optional flat +1 so the rounded-down result acts like rounding up.
        ...(config.roundUp ? [{ code: 'regen', min: 1, max: 1 }] : []),
    ];
}

// ---------------------------------------------------------------------------
// 1. Stats (itemstatcost.txt). Order matters: these become stat IDs.
// ---------------------------------------------------------------------------
{
    const path = 'global\\excel\\itemstatcost.txt';
    const isc = D2RMM.readTsv(path);
    const base = isc.rows.find((r) => r.Stat === 'item_hp_perlevel');
    const regen = isc.rows.find((r) => r.Stat === 'hpregen');
    const idCol = isc.headers.find((h) => h === '*ID' || h === 'ID');
    const saveBits = findCol(isc.headers, 'Save Bits');
    const sendBits = findCol(isc.headers, 'Send Bits');
    const valShift = findCol(isc.headers, 'ValShift');
    let nextId = 1 + Math.max(...isc.rows.map((r) => parseInt(r[idCol], 10) || 0));
    const priority = parseInt(regen.descpriority, 10);

    const blank = {
        op: '',
        'op param': '',
        'op base': '',
        'op stat1': '',
        'op stat2': '',
        'op stat3': '',
        descfunc: '',
        descval: '',
        descstrpos: '',
        descstrneg: '',
        descstr2: '',
        descpriority: '',
        dgrp: '',
        dgrpfunc: '',
        dgrpval: '',
        dgrpstrpos: '',
        dgrpstrneg: '',
        dgrpstr2: '',
    };
    // Room for N * 41 (13 signed bits) and no x256 on the stored value.
    const wide = {};
    if (saveBits) wide[saveBits] = 13;
    if (sendBits) wide[sendBits] = 13;
    if (valShift) wide[valShift] = 0;

    const add = (fields) => {
        const row = { ...base, ...blank, ...fields };
        if (idCol) row[idCol] = nextId++;
        isc.rows.push(row);
    };

    // Hidden real stat: adds the regen. No tooltip line.
    add({
        ...wide,
        Stat: STAT_REAL,
        op: 2,
        'op param': SHIFT,
        'op base': 'maxhp',
        'op stat1': 'hpregen',
    });
    // "+N% of Max Life as Replenish Life". No effect.
    add({
        Stat: STAT_DISPLAY,
        descfunc: 4,
        descval: 1,
        descstrpos: DESC_PCT,
        descstrneg: DESC_PCT,
        descpriority: priority + 2, // higher shows first: % line, total, then Replenish Life +1
    });
    // Dummy target so the total line's op does nothing.
    add({ Stat: STAT_DUMMY });
    // "+X Replenish Life (Based on Max Life)": shows the current total.
    add({
        ...wide,
        Stat: STAT_TOTAL,
        op: 2,
        'op param': SHIFT,
        'op base': 'maxhp',
        'op stat1': STAT_DUMMY,
        descfunc: 1,
        descval: 1,
        descstrpos: DESC_TOTAL,
        descstrneg: DESC_TOTAL,
        descpriority: priority + 1,
    });
    D2RMM.writeTsv(path, isc);
}

// ---------------------------------------------------------------------------
// 2. Properties (properties.txt), cloned from "regen" (func 1 = set stat)
// ---------------------------------------------------------------------------
{
    const path = 'global\\excel\\properties.txt';
    const props = D2RMM.readTsv(path);
    const regen = props.rows.find((r) => r.code === 'regen');
    props.rows.push({ ...regen, code: PROP_REAL, stat1: STAT_REAL, func2: regen.func1, stat2: STAT_TOTAL });
    props.rows.push({ ...regen, code: PROP_DISPLAY, stat1: STAT_DISPLAY });
    D2RMM.writeTsv(path, props);

    const mods = 'local\\lng\\strings\\item-modifiers.json';
    addString(mods, DESC_PCT, 'of Max Life as Replenish Life');
    addString(mods, DESC_TOTAL, 'Replenish Life (Based on Max Life)');
}

// ---------------------------------------------------------------------------
// 3. Suffix tiers (magicsuffix.txt): magic Large Charms.
//    All three rows are always written, in the same order, so suffix IDs
//    never move; a disabled tier gets spawnable 0.
// ---------------------------------------------------------------------------
{
    const path = 'global\\excel\\magicsuffix.txt';
    const suffixes = D2RMM.readTsv(path);
    // One shared group: a charm gets at most one of the tiers, and the tiers
    // never block other suffixes.
    const group = 1 + Math.max(...suffixes.rows.map((r) => parseInt(r.group, 10) || 0));
    TIERS.forEach((tier) => {
        const row = blankRow(suffixes.headers);
        Object.assign(row, {
            Name: tier.key,
            version: 100,
            spawnable: tier.enabled ? 1 : 0,
            rare: 1,
            level: tier.level,
            levelreq: tier.levelReq,
            frequency: tier.frequency,
            group,
            transformcolor: 'dred',
            itype1: 'mcha', // ItemTypes: scha = Small, mcha = Large, lcha = Grand Charm
            multiply: 0,
            add: 0,
        });
        regenMods(tier.pct).forEach((m, i) => {
            row[`mod${i + 1}code`] = m.code;
            row[`mod${i + 1}min`] = m.min;
            row[`mod${i + 1}max`] = m.max;
        });
        suffixes.rows.push(row);
    });
    D2RMM.writeTsv(path, suffixes);

    const names = 'local\\lng\\strings\\item-nameaffixes.json';
    TIERS.forEach((tier) => addString(names, tier.key, tier.name));
}

// ---------------------------------------------------------------------------
// 4. Unique Large Charm on its own base item, optionally sold by Gheed.
//    The item always exists so charms stay valid if the option is turned off;
//    the option only controls whether Gheed stocks it.
// ---------------------------------------------------------------------------
{
    const miscPath = 'global\\excel\\misc.txt';
    const misc = D2RMM.readTsv(miscPath);

    // Pick a 3-letter code no other item uses.
    const used = new Set(misc.rows.map((r) => r.code));
    ['weapons', 'armor'].forEach((f) => {
        D2RMM.readTsv(`global\\excel\\${f}.txt`).rows.forEach((r) => used.add(r.code));
    });
    const code = ['cl9', 'cl8', 'cl7', 'cl6', 'cl5'].find((c) => !used.has(c));

    const largeCharm = misc.rows.find((r) => r.code === 'cm2');
    const item = { ...largeCharm, name: 'Large Charm (Life Regen Unique)', code, unique: 1, level: 1, cost: 0, 'gamble cost': 0 };
    const set = (name, value) => {
        const col = findCol(misc.headers, name);
        if (col) item[col] = value;
    };
    const gheed = config.soldByGheed ? 1 : 0;
    set('GheedMin', gheed);
    set('GheedMax', gheed);
    set('GheedMagicMin', gheed);
    set('GheedMagicMax', gheed);
    set('GheedMagicLevel', config.soldByGheed ? 255 : 0);
    set('GheedMagicLvl', config.soldByGheed ? 255 : 0);
    set('PermStoreItem', config.soldByGheed ? 1 : 0);
    set('multibuy', 1);
    set('NightmareUpgrade', 'xxx');
    set('HellUpgrade', 'xxx');
    misc.rows.push(item);
    D2RMM.writeTsv(miscPath, misc);

    // Same graphic as the regular Large Charm.
    const itemsPath = 'hd\\items\\items.json';
    const items = D2RMM.readJson(itemsPath);
    const cm2 = items.find((e) => e.cm2);
    items.push({ [code]: cm2 ? cm2.cm2 : { asset: 'charm/charm_medium' } });
    D2RMM.writeJson(itemsPath, items);

    const uniqPath = 'global\\excel\\uniqueitems.txt';
    const uniq = D2RMM.readTsv(uniqPath);
    const idCol = uniq.headers.find((h) => h === '*ID' || h === 'ID');
    // Clone an existing unique charm so every column the game expects is
    // filled in, then clear its stats and limits.
    const template = uniq.rows.find((r) => r.index === 'Annihilus') || uniq.rows.find((r) => r.code === 'cm2');
    const row = { ...template };
    for (let i = 1; i <= 12; i++) {
        row[`prop${i}`] = '';
        row[`par${i}`] = '';
        row[`min${i}`] = '';
        row[`max${i}`] = '';
    }
    Object.assign(row, {
        index: UNIQUE_KEY,
        carry1: '',
        lvl: 1,
        'lvl req': config.uniqueLevelReq,
        code,
        '*ItemName': 'Large Charm',
        'cost mult': 1,
        'cost add': 1,
    });
    if (idCol) row[idCol] = 1 + Math.max(...uniq.rows.map((r) => parseInt(r[idCol], 10) || 0));
    regenMods(config.uniquePct).forEach((m, i) => {
        row[`prop${i + 1}`] = m.code;
        row[`min${i + 1}`] = m.min;
        row[`max${i + 1}`] = m.max;
    });
    uniq.rows.push(row);
    D2RMM.writeTsv(uniqPath, uniq);
    addString('local\\lng\\strings\\item-names.json', UNIQUE_KEY, config.uniqueName);
}
