// Runs the conformance suite in headless Chromium, one page per profile.
// usage: node conformance/run.mjs [profile …] [--tests=T1,T2] [--record] [--headed] [--suite=smoke]
import { chromium } from "playwright";
import { spawn } from "node:child_process";
import fs from "node:fs";
import { startWorkerProfiles, summarize } from "./cdp-prof.mjs";

const args = process.argv.slice(2);
const opt = k => (args.find(a => a.startsWith(`--${k}=`)) ?? "").split("=")[1];
const profiles = args.filter(a => !a.startsWith("--"));
const all = fs.readdirSync("profiles").filter(f => f.endsWith(".json")).map(f => f.slice(0, -5));
const PORT = 8780;
const server = spawn("node", ["tools/serve.mjs", ".", String(PORT)], { stdio: "ignore" });
await new Promise(r => setTimeout(r, 500));
fs.mkdirSync("conformance/results", { recursive: true });

const browser = await chromium.launch({ headless: !args.includes("--headed"),
  args: ["--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
    ...(args.includes("--cpuprofile") ? ["--remote-debugging-port=9333"] : [])] });
const summary = {};
for (const id of profiles.length ? profiles : all) {
  const t0 = Date.now();
  const page = await browser.newPage({ viewport: { width: 1000, height: 900 } });
  let stopProf = null;
  page.on("console", async m => {
    console.log(`[${id} +${((Date.now() - t0) / 1000).toFixed(0)}s]`, m.text());
    if (m.text() === "PROF start" && args.includes("--cpuprofile")) stopProf = await startWorkerProfiles(9333);
    if (m.text() === "PROF stop" && stopProf) {
      for (const [i, w] of (await stopProf()).entries()) {
        fs.writeFileSync(`conformance/results/${id}-worker${i}.cpuprofile`, JSON.stringify(w.profile));
        const s = summarize(w.profile);
        console.log(`\n=== worker ${i} ${w.url.split("/").pop()} sampled ${s.total_ms.toFixed(0)}ms ===\n` + s.rows.join("\n"));
      }
    }
  });
  page.on("worker", w => w.on("console", m => console.log(`[${id} worker]`, m.text())));
  page.on("pageerror", e => console.log(`[${id}] pageerror`, e.stack || e.message));
  const q = new URLSearchParams({ profile: id, suite: opt("suite") ?? "conformance", tests: opt("tests") ?? "all", record: args.includes("--record") ? "1" : "", realclock: args.includes("--realclock") ? "1" : "", steps: opt("steps") ?? "", nopoll: opt("nopoll") ?? "", secs: opt("secs") ?? "", debug: opt("debug") ?? "", bakewait: opt("bakewait") ?? "", overlay: opt("overlay") ?? "", noready: opt("noready") ?? "", prepare: opt("prepare") ?? "" });
  await page.goto(`http://localhost:${PORT}/conformance/index.html?${q}`);
  await page.waitForFunction(() => window.RESULTS?.done, null, { timeout: 30 * 60_000, polling: 1000 });
  const res = await page.evaluate(() => window.RESULTS);
  res.browser = browser.version(); res.wall_s = (Date.now() - t0) / 1000;
  fs.writeFileSync(`conformance/results/${id}.json`, JSON.stringify(res, null, 1));
  summary[id] = res.tests;
  await page.close();
}
await browser.close();
server.kill();

const ids = Object.keys(summary);
const tests = [...new Set(ids.flatMap(i => Object.keys(summary[i])))];
console.log("\n" + ["test", ...ids].join(" | "));
for (const t of tests) console.log([t, ...ids.map(i => summary[i][t] ? (summary[i][t].pass ? "PASS" : "FAIL") : "-")].join(" | "));
process.exit(ids.every(i => Object.values(summary[i]).every(t => t.pass)) ? 0 : 1);
