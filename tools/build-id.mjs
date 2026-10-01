// Print "sha256:<hex>" for a core's wasm file: the build id that profiles and snapshots carry.
import fs from "node:fs";
import crypto from "node:crypto";
console.log("sha256:" + crypto.createHash("sha256").update(fs.readFileSync(process.argv[2])).digest("hex"));
