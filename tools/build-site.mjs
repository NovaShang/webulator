// Assemble the static demo site in site/: page, library build, profiles, and only the assets the profiles use.
// Snapshots come from conformance runs (uploads/<profile>/s0.webusnap).
import fs from "node:fs";
import path from "node:path";

const OUT = "site";
fs.rmSync(OUT, { recursive: true, force: true });
const copy = (from, to = from) => { fs.mkdirSync(path.dirname(path.join(OUT, to)), { recursive: true }); fs.cpSync(from, path.join(OUT, to), { recursive: true }); };

for (const f of ["index.html", "demo.js", "coi-sw.js"]) copy(`demo/${f}`, f);
copy("dist/webulator.js", "dist/webulator.js");
copy("dist/webulator-worker.js", "dist/webulator-worker.js");
fs.writeFileSync(path.join(OUT, ".nojekyll"), "");

const used = new Set();
for (const f of fs.readdirSync("profiles").filter(f => f.endsWith(".json"))) {
  copy(`profiles/${f}`);
  const p = JSON.parse(fs.readFileSync(`profiles/${f}`, "utf8"));
  const rel = u => path.normalize(path.join("profiles", u));
  used.add(rel(p.core.module)); if (p.core.wasm) used.add(rel(p.core.wasm));
  Object.values(p.machine.files).forEach(u => used.add(rel(u)));
  for (const d of p.disks) used.add(path.dirname(rel(d.source.manifest)));
  const snap = `uploads/${p.id}/s0.webusnap`;
  if (fs.existsSync(snap)) copy(snap, `snapshots/${p.id}.webusnap`); else console.warn(`no snapshot for ${p.id}`);
}
for (const u of used) copy(u);
// Mini vMac / macemu modules load their .wasm next to the .mjs.
for (const u of [...used].filter(u => u.endsWith(".mjs"))) { const w = u.replace(/\.mjs$/, ".wasm"); if (fs.existsSync(w)) copy(w); }

let files = 0, bytes = 0;
const walk = d => fs.readdirSync(d, { withFileTypes: true }).forEach(e => { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else { files++; bytes += fs.statSync(p).size; } });
walk(OUT);
console.log(`site/: ${files} files, ${(bytes / 1048576).toFixed(0)} MB`);
