// Builds dist/webulator.js (main thread) and dist/webulator-worker.js (one worker per machine).
import { build } from "esbuild";

const common = { bundle: true, format: "esm", target: "es2022", sourcemap: true, logLevel: "warning" };
await Promise.all([
  build({ ...common, entryPoints: ["src/index.ts"], outfile: "dist/webulator.js" }),
  build({ ...common, entryPoints: ["src/worker/main.ts"], outfile: "dist/webulator-worker.js" }),
]);
console.log("built dist/webulator.js, dist/webulator-worker.js");
