// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Runs an exported mod.ts the way D2RMM does on a game data folder and writes the tables it
// changed to <out>/<same path> (a stand-in for D2RMM's merged mod).
// usage: node merge_d2rmm.mjs <data folder> <mod.ts> <out folder>
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [data, mod, out] = process.argv.slice(2);
const local = (p) => {
  const parts = p.split('\\');
  let at = data;
  for (const part of parts) {
    const found = fs.existsSync(at) ? fs.readdirSync(at).find((f) => f.toLowerCase() === part.toLowerCase()) : undefined;
    if (!found) return null;
    at = path.join(at, found);
  }
  return at;
};
const parse = (text) => {
  const lines = text.replace(/^﻿/, '').split('\n').map((l) => l.replace(/\r$/, ''));
  if (lines[lines.length - 1] === '') lines.pop();
  const headers = lines[0].split('\t');
  return { headers, rows: lines.slice(1).map((l) => { const c = l.split('\t'); return Object.fromEntries(headers.map((h, i) => [h, c[i] ?? ''])); }) };
};
const write = (p, text) => {
  const file = path.join(out, ...p.split('\\'));
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, text);
};
let nextId = 900000;
globalThis.D2RMM = {
  readTsv: (p) => { const f = local(p); return f ? parse(fs.readFileSync(f, 'latin1')) : null; },
  writeTsv: (p, t) => write(p, [t.headers.join('\t'), ...t.rows.map((r) => t.headers.map((h) => r[h] ?? '').join('\t'))].join('\r\n') + '\r\n'),
  readJson: (p) => { const f = local(p); return f ? JSON.parse(fs.readFileSync(f, 'utf8').replace(/^﻿/, '')) : []; },
  writeJson: (p, j) => write(p, JSON.stringify(j)),
  getNextStringID: () => nextId++,
};
const warnings = [];
console.warn = (w) => warnings.push(w);
await import(pathToFileURL(path.resolve(mod)).href);
console.log(warnings.length === 0 ? 'merged, no warnings' : `merged, warnings: ${warnings.join(' | ')}`);
