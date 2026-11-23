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
  fs.copyFileSync(path.join(work, 'mod.ts'), path.join(work, 'mod-again.ts')); // a module runs once per file
  await import(pathToFileURL(path.join(work, 'mod-again.ts')).href);
} catch (e) {
  stopped = String(e.message);
}
check(stopped.includes('stat 478') && stopped.includes('another mod uses that number'), `D2RMM: a stat number another mod took stops the mod (${stopped.slice(0, 80)})`);
console.log(failed === 0 ? 'ALL PASSED' : `FAILURES: ${failed}`);
process.exit(failed === 0 ? 0 : 1);
