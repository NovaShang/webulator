// Open a benchmark page in Chromium, wait for window.RESULTS.done, save the results.
// usage: node run-page.mjs <url> <out.json> [timeout_s] [--headed]
import { chromium } from "playwright";
import fs from "node:fs";
const [url, outFile, timeoutS = "1800"] = process.argv.slice(2);
const headed = process.argv.includes("--headed");
const browser = await chromium.launch({ headless: !headed, args: ["--disable-background-timer-throttling", "--disable-renderer-backgrounding"] });
const t0 = Date.now();
const page = await browser.newPage({ viewport: { width: 1000, height: 800 } });
page.on("console", m => console.log(`[page +${((Date.now() - t0) / 1000).toFixed(0)}s]`, m.text()));
const snap = setInterval(() => page.screenshot({ path: outFile.replace(/\.json$/, ".live.png") }).catch(() => {}), 20000);
await page.goto(url);
await page.waitForFunction(() => window.RESULTS && window.RESULTS.done, null, { timeout: +timeoutS * 1000, polling: 2000 });
const res = await page.evaluate(() => window.RESULTS);
res.browser = browser.version(); res.url = url; res.wall_s = (Date.now() - t0) / 1000;
fs.writeFileSync(outFile, JSON.stringify(res, null, 1));
await page.screenshot({ path: outFile.replace(/\.json$/, ".png") });
console.log(JSON.stringify(res.runs));
clearInterval(snap);
await browser.close();
