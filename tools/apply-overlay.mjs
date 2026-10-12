// Merge an exported disk overlay into a copy of the base image: node tools/apply-overlay.mjs <base> <overlay> <out>
// Writes block by block into the copy, so images larger than Node's 2 GB readFileSync limit work.
import fs from "node:fs";
const [base, ovFile, out] = process.argv.slice(2);
fs.copyFileSync(base, out);
const ov = fs.readFileSync(ovFile), fd = fs.openSync(out, "r+");
const bs = ov.readUInt32LE(0), n = ov.readUInt32LE(4);
for (let i = 0; i < n; i++) {
  const blk = ov.readUInt32LE(8 + i * 4), at = 8 + n * 4 + i * bs;
  fs.writeSync(fd, ov, at, bs, blk * bs);
}
fs.closeSync(fd);
console.log(`${n} blocks of ${bs} B applied → ${out}`);
