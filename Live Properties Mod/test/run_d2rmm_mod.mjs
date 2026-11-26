// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Runs a D2RMM mod.js (read only) on COPIES of tables: a small stand-in for D2RMM readTsv/writeTsv, default config.
//   node run_d2rmm_mod.mjs <mod folder> <tables in> <tables out>
import fs from 'fs'; import path from 'path'; import vm from 'vm';
const [,, dir, inDir, outDir] = process.argv;
const meta = JSON.parse(fs.readFileSync(path.join(dir,'mod.json'),'utf8'));
const config = {}; const walk = (items) => { for (const i of items||[]) { if (i.children) walk(i.children); else if (i.id) config[i.id] = i.defaultValue; } }; walk(meta.config);
const files = {}; const name = (f) => path.basename(f.replace(/\\/g,'/')).toLowerCase();
const D2RMM = {
  readTsv(f) { if (f.includes('base\\')) return null; const p = path.join(inDir, name(f)); if (!fs.existsSync(p)) return null; const lines = fs.readFileSync(p,'latin1').split(/\r?\n/).filter(l=>l.length); const headers = lines[0].split('\t'); return { headers, rows: lines.slice(1).map(l=>{ const c=l.split('\t'); const r={}; headers.forEach((h,i)=>r[h]=c[i]??''); return r; }) }; },
  writeTsv(f, d) { if (f.includes('base\\')) return; files[name(f)] = d; },
  readJson(){return null;}, writeJson(){}, readTxt(){return null;}, writeTxt(){}, copyFile(){}, getVersion(){return 1.8;}, getConfigJSON(){return config;}, error(e){console.error(e);}
};
vm.runInNewContext(fs.readFileSync(path.join(dir,'mod.js'),'utf8'), { D2RMM, config, console, Math, JSON, Object, Array, Number, String, parseInt, parseFloat, isNaN, Set, Map });
for (const [n, d] of Object.entries(files)) { const out = [d.headers.join('\t'), ...d.rows.map(r=>d.headers.map(h=>r[h]??'').join('\t'))].join('\r\n')+'\r\n'; fs.writeFileSync(path.join(outDir, n), out, 'latin1'); console.log(n, d.rows.length, 'rows'); }
