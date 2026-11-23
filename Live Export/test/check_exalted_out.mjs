// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// Checks the cubemain Exalted Items' generator made on an exported mod (check_exalted.sh).
// usage: node check_exalted_out.mjs <cubemain> <expect.txt> <generator log> <which export>
import fs from "node:fs";
const [file, expect, log, way] = process.argv.slice(2);
const [up, down, helm, body] = fs.readFileSync(expect, "utf8").split("\n");
const lines = fs.readFileSync(file, "latin1").split("\r\n").filter((l) => l);
const head = lines[0].split("\t");
const rows = lines.slice(1).map((l) => Object.fromEntries(l.split("\t").map((c, i) => [head[i], c])));
const x = rows.filter((r) => r.description.startsWith("[xlt] "));
const bad = [];
const kept = fs.readFileSync(log, "utf8").match(/kept (\d+) upgrade, (\d+) downgrade and (\d+) restore recipes from the mod/);
if (!kept || kept.slice(1).join() !== "1,1,2") bad.push("log: " + (kept ? kept[0] : "no kept line"));
if (!x.some((r) => r["input 1"] === up && r["input 2"] === "r33")) bad.push("the changed upgrade is not kept");
if (!x.some((r) => r["input 1"] === down && r["input 2"] === "r05")) bad.push("the changed back-to-elite recipe is not kept");
if (!x.some((r) => r.description === body)) bad.push("the changed restore recipe is not kept");
const helms = x.filter((r) => r["input 1"] === helm && r.description.includes("restored: Helms"));
if (helms.length !== 1 || helms[0].enabled !== "0") bad.push("the removed restore recipe is not there once with enabled 0 (" + helms.length + ")");
if (x.length !== 344) bad.push(x.length + " Exalted recipes, 344 wanted (one per key)");
console.log((bad.length ? "FAIL" : "ok  ") + "  " + way + ": Exalted Items' generator keeps the 3 changed recipes, the removed one stays off, it makes the other " + (x.length - 4) + " " + bad.join("; "));
process.exit(bad.length ? 1 : 0);
