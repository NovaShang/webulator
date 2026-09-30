// Merge an exported disk overlay into a copy of the base image: node tools/apply-overlay.mjs <base> <overlay> <out>
import fs from "node:fs";
const [base, ovFile, out] = process.argv.slice(2);
const img = fs.readFileSync(base);
const ov = fs.readFileSync(ovFile);
const bs = ov.readUInt32LE(0), n = ov.readUInt32LE(4);
for (let i = 0; i < n; i++) {
  const blk = ov.readUInt32LE(8 + i * 4);
  ov.copy(img, blk * bs, 8 + n * 4 + i * bs, 8 + n * 4 + (i + 1) * bs);
}
fs.writeFileSync(out, img);
console.log(`${n} blocks of ${bs} B applied → ${out}`);
