// Visit the demo like a user: cross-origin isolation via the service worker, wake every machine, save and restore.
// usage: node tools/check-site.mjs <url> [outdir]
import { chromium } from "playwright";
import fs from "node:fs";
const [url, out = "uploads/site-check"] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ args: ["--disable-background-timer-throttling", "--disable-renderer-backgrounding"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 1000 } });
page.on("pageerror", e => console.log("pageerror", e.message));
const statusText = () => page.textContent("#statusText");
const waitRunning = async (timeout = 180000) => {
  await page.waitForFunction(() => /^Running/.test(document.getElementById("statusText").textContent), null, { timeout, polling: 250 });
  return statusText();
};
const nonBlank = () => page.evaluate(() => {
  const c = document.getElementById("screen"); const d = c.getContext("2d").getImageData(0, 0, c.width, c.height).data;
  let lit = 0; for (let i = 0; i < d.length; i += 16) lit += d[i] + d[i + 1] + d[i + 2] > 60; return { w: c.width, h: c.height, lit };
});
const results = {};
await page.goto(url);
await page.waitForFunction(() => window.crossOriginIsolated, null, { timeout: 30000 });
console.log("crossOriginIsolated after service worker reload");
for (const id of ["basilisk2-quadra650-system753", "minivmac-plus-system608", "sheepshaver-g3bw-macos904", "v86-win98"]) {
  const t0 = Date.now();
  await page.click(`.machine[data-id="${id}"]`);
  await page.waitForFunction(() => !/^Running/.test(document.getElementById("statusText").textContent), null, { timeout: 10000, polling: 20 }).catch(() => {});
  await page.waitForFunction(() => /restored in/.test(document.getElementById("statusText").textContent), null, { timeout: 180000, polling: 250 });
  const st = await statusText();
  await page.waitForTimeout(2500);
  const px = await nonBlank();
  await page.locator(".stage").screenshot({ path: `${out}/${id}.png` });
  results[id] = { status: st, wall_s: (Date.now() - t0) / 1000, ...px };
  console.log(id, JSON.stringify(results[id]));
}
// Save and restore on the last machine.
await page.click("#save");
await page.waitForFunction(() => /saved/.test(document.getElementById("statusText").textContent), null, { timeout: 30000 });
console.log("save:", await statusText());
await page.click("#restore");
await page.waitForFunction(() => /restored/.test(document.getElementById("statusText").textContent), null, { timeout: 60000 });
console.log("restore:", await statusText());
await page.screenshot({ path: `${out}/page.png` });
fs.writeFileSync(`${out}/results.json`, JSON.stringify(results, null, 1));
await browser.close();
