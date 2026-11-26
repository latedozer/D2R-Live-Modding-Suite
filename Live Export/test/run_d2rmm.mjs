// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Runs the exported mod.ts the way D2RMM does (a global D2RMM with readTsv / writeTsv / readJson /
// writeJson / getNextStringID) on the game's own tables, and compares the tables it writes with
// what the .mpq export made of the same edits (want-*.txt from export_test).
// usage: node run_d2rmm.mjs <excel folder> <work folder>
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [excel, work] = process.argv.slice(2);
const files = Object.fromEntries(fs.readdirSync(excel).map((f) => [f.toLowerCase(), path.join(excel, f)]));
const written = {};
let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`);
  failed += ok ? 0 : 1;
};

const parse = (text) => {
  const lines = text.replace(/^﻿/, '').split('\n').map((l) => l.replace(/\r$/, ''));
  if (lines[lines.length - 1] === '') lines.pop();
  const headers = lines[0].split('\t');
  const rows = lines.slice(1).map((l) => {
    const cells = l.split('\t');
    return Object.fromEntries(headers.map((h, i) => [h, cells[i] ?? '']));
  });
  return { headers, rows };
};
const strings = [{ id: 10, Key: 'Stout', enUS: 'Stout' }];
let nextId = 97031;
globalThis.D2RMM = {
  readTsv: (p) => {
    const name = p.split('\\').pop().toLowerCase();
    return p.includes('\\base\\') || !files[name] ? null : parse(fs.readFileSync(files[name], 'utf8'));
  },
  writeTsv: (p, t) => (written[p.split('\\').pop().toLowerCase()] = t),
  readJson: () => strings,
  writeJson: (p, j) => (written[p.split('\\').pop()] = j),
  getNextStringID: () => nextId++,
};
const warnings = [];
console.warn = (w) => warnings.push(w);
await import(pathToFileURL(path.join(work, 'mod.ts')).href);

const same = (file, wantFile) => {
  const want = parse(fs.readFileSync(path.join(work, wantFile), 'utf8'));
  const got = written[file];
  if (!got) return `${file} was not written`;
  if (got.rows.length !== want.rows.length) return `${got.rows.length} rows, ${want.rows.length} wanted`;
  for (let r = 0; r < want.rows.length; r++) {
    for (const h of want.headers) {
      if ((got.rows[r][h] ?? '') !== (want.rows[r][h] ?? '')) return `row ${r} ${h}: '${got.rows[r][h]}' not '${want.rows[r][h]}'`;
    }
  }
  return '';
};
for (const [file, want] of [
  ['properties.txt', 'want-properties.txt'],
  ['itemstatcost.txt', 'want-itemstatcost.txt'],
  ['magicprefix.txt', 'want-magicprefix.txt'],
  ['cubemain.txt', 'want-cubemain.txt'],
]) {
  const why = same(file, want);
  check(why === '', `D2RMM: ${file} comes out as the .mpq export's ${why}`);
}
{
  const isc = written['itemstatcost.txt'].rows, pre = written['magicprefix.txt'].rows, props = written['properties.txt'].rows;
  check(isc[1].Stat === 'energy' && isc[1].op === '' && isc[1]['op base'] === '' && isc[1].descfunc === '' && isc[1].dgrp === '', 'D2RMM: the disabled stat energy has no op, description or item event');
  check(isc[368].Stat === 'manaperlife' && isc[368]['Save Bits'] === '13' && isc[368].Add === '7' && isc[369].Stat === 'grown_only' && isc[369]['Save Bits'] === '9', 'D2RMM: the made stat with its edited save bits (13, not the grown 20), the grown stat without a card line');
  check(pre[17].spawnable === '0' && pre[17].frequency === '0' && pre[135].spawnable === '0', 'D2RMM: the disabled affixes Savage and Stout roll nowhere (spawnable 0, frequency 0)');
  check(props.some((r) => r.code === 'grown-prop' && r.func1 === '17' && r.stat1 === 'grown_only'), 'D2RMM: the grown property with its cells');
}
const named = (written['item-nameaffixes.json'] ?? []).find((s) => s.Key === 'live_magicprefix_9');
check(named?.enUS === 'Tir-forged' && named.id === 97031, 'D2RMM: the made prefix\'s name is added to item-nameaffixes.json with the next string id');
check(warnings.length === 0, `D2RMM: no warnings ${warnings.join(' | ')}`);

// a stat number another mod already took: the mod stops instead of writing over it
globalThis.D2RMM.readTsv = (p) => {
  const name = p.split('\\').pop().toLowerCase();
  if (name !== 'itemstatcost.txt') return null;
  const t = parse(fs.readFileSync(files[name], 'utf8'));
  while (t.rows.length <= 478) t.rows.push(Object.fromEntries(t.headers.map((h) => [h, h === 'Stat' ? `othermod_${t.rows.length}` : ''])));
  return t;
};
let stopped = '';
try {
  for (const k of Object.keys(written)) delete written[k];
  fs.copyFileSync(path.join(work, 'mod.ts'), path.join(work, 'mod-again.ts')); // a module runs once per file
  await import(pathToFileURL(path.join(work, 'mod-again.ts')).href);
} catch (e) {
  stopped = String(e.message);
}
check(stopped.includes('stat 368') && stopped.includes('another mod uses that number') && Object.keys(written).length === 0, `D2RMM: a stat number another mod took stops the mod before it writes anything (${stopped.slice(0, 80)})`);

// the game run of 2026-11-25: made stats 389/390 on Exalted's packed 389-row table
const runPacked = async (rows, file) => {
  for (const k of Object.keys(written)) delete written[k];
  globalThis.D2RMM.readTsv = (p) => {
    if (p.split('\\').pop().toLowerCase() !== 'itemstatcost.txt' || p.includes('\\base\\')) return null;
    const t = parse(fs.readFileSync(path.join(work, 'packed-itemstatcost.txt'), 'utf8'));
    t.rows.length = Math.min(t.rows.length, rows);
    while (t.rows.length < rows) t.rows.push(Object.fromEntries(t.headers.map((h) => [h, h === 'Stat' ? `othermod_${t.rows.length}` : ''])));
    return t;
  };
  fs.copyFileSync(path.join(work, 'mod-packed.ts'), path.join(work, file));
  try {
    await import(pathToFileURL(path.join(work, file)).href);
    return '';
  } catch (e) {
    return String(e.message);
  }
};
let why = await runPacked(389, 'mod-packed-1.ts');
const packed = same('itemstatcost.txt', 'want-packed-itemstatcost.txt');
check(why === '' && packed === '' && !written['itemstatcost.txt'].rows.some((r) => r.Stat.startsWith('live_unused_')), `D2RMM: stats 389/390 go right after a 389-row table, no filler rows ${why}${packed}`);
why = await runPacked(380, 'mod-packed-2.ts');
check(why.includes('ends at 379') && why.includes('No filler rows') && Object.keys(written).length === 0, `D2RMM: a shorter stat table stops the mod, nothing written (${why.slice(0, 90)})`);
why = await runPacked(395, 'mod-packed-3.ts');
check(why.includes("stat 389 is 'othermod_389'") && Object.keys(written).length === 0, `D2RMM: another mod's stat at 389 stops the mod, nothing written (${why.slice(0, 90)})`);
console.log(failed === 0 ? 'ALL PASSED' : `FAILURES: ${failed}`);
process.exit(failed === 0 ? 0 : 1);
