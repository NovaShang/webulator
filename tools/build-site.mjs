// Assemble the static demo site in site/: page, library build, profiles, and only the assets the profiles use.
// Snapshots come from conformance runs (uploads/<profile>/s0.webusnap).
import fs from "node:fs";
import path from "node:path";

const OUT = "site";
// Disk images (the bulk of the site) can live elsewhere, e.g. an R2 bucket: see site.config.json and
// tools/upload-disks-r2.sh. Then the site only carries code, cores, ROMs and snapshots.
const config = fs.existsSync("site.config.json") ? JSON.parse(fs.readFileSync("site.config.json", "utf8")) : {};
fs.rmSync(OUT, { recursive: true, force: true });
const copy = (from, to = from) => { fs.mkdirSync(path.dirname(path.join(OUT, to)), { recursive: true }); fs.cpSync(from, path.join(OUT, to), { recursive: true }); };

for (const f of ["index.html", "demo.js", "coi-sw.js"]) copy(`demo/${f}`, f);
copy("dist/webulator.js", "dist/webulator.js");
copy("dist/webulator-worker.js", "dist/webulator-worker.js");
fs.writeFileSync(path.join(OUT, ".nojekyll"), "");
// Disk chunks are binary; never let git rewrite line endings in them (core.autocrlf would corrupt some).
fs.writeFileSync(path.join(OUT, ".gitattributes"), "* -text\n");

const used = new Set();
for (const f of fs.readdirSync("profiles").filter(f => f.endsWith(".json"))) {
  const p = JSON.parse(fs.readFileSync(`profiles/${f}`, "utf8"));
  const rel = u => path.normalize(path.join("profiles", u));
  used.add(rel(p.core.module)); if (p.core.wasm) used.add(rel(p.core.wasm));
  Object.values(p.machine.files).forEach(u => used.add(rel(u)));
  for (const d of p.disks) {
    const local = rel(d.source.manifest);                       // assets/disks/<name>/manifest.json
    if (config.diskBase) d.source.manifest = new URL(path.relative("assets", local), config.diskBase).href;
    else used.add(path.dirname(local));
  }
  fs.mkdirSync(path.join(OUT, "profiles"), { recursive: true });
  fs.writeFileSync(path.join(OUT, "profiles", f), JSON.stringify(p, null, 2));
  const snap = `uploads/${p.id}/s0.webusnap`;
  if (fs.existsSync(snap)) copy(snap, `snapshots/${p.id}.webusnap`); else console.warn(`no snapshot for ${p.id}`);
}
for (const u of used) copy(u);
// Mini vMac / macemu modules load their .wasm next to the .mjs.
for (const u of [...used].filter(u => u.endsWith(".mjs"))) { const w = u.replace(/\.mjs$/, ".wasm"); if (fs.existsSync(w)) copy(w); }

let files = 0, bytes = 0;
const walk = d => fs.readdirSync(d, { withFileTypes: true }).forEach(e => { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else { files++; bytes += fs.statSync(p).size; } });
walk(OUT);
console.log(`site/: ${files} files, ${(bytes / 1048576).toFixed(0)} MB${config.diskBase ? `; disks from ${config.diskBase}` : ""}`);
