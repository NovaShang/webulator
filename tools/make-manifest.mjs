// Split a disk image into content-addressed 256 KB chunks plus a manifest (spec §6.1). All-zero chunks are omitted.
// usage: node tools/make-manifest.mjs <image> <outdir> [name]
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";

const [img, out, name] = process.argv.slice(2);
const CS = 256 * 1024;
const buf = fs.readFileSync(img);
fs.mkdirSync(path.join(out, "chunks"), { recursive: true });
const chunks = [];
for (let off = 0; off < buf.length; off += CS) {
  const c = buf.subarray(off, Math.min(off + CS, buf.length));
  if (c.every(b => b === 0)) { chunks.push(""); continue; }
  const h = crypto.createHash("sha256").update(c).digest("hex");
  const f = path.join(out, "chunks", h);
  if (!fs.existsSync(f)) fs.writeFileSync(f, c);
  chunks.push(h);
}
const manifest = { format: "webulator-disk/1", name: name ?? path.basename(img), size: buf.length, chunkSize: CS, chunks, baseUrl: "chunks/" };
fs.writeFileSync(path.join(out, "manifest.json"), JSON.stringify(manifest));
console.log(`${img}: ${chunks.length} chunks, ${chunks.filter(Boolean).length} stored`);
