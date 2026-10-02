// In-page test harness. Every test drives the public API only.
import { Machine, loadProfile } from "/dist/webulator.js";

const Q = new URLSearchParams(location.search);
const ID = Q.get("profile"), SUITE = Q.get("suite") || "conformance";
const ONLY = (Q.get("tests") || "all") === "all" ? null : new Set(Q.get("tests").split(","));
const RECORD = Q.get("record") === "1";
const out = document.getElementById("out");
const T0 = performance.now();
export const log = (...a) => { const l = `+${((performance.now() - T0) / 1000).toFixed(1)}s ${a.join(" ")}`; out.textContent += l + "\n"; console.log(l); };
window.RESULTS = { profile: ID, suite: SUITE, tests: {}, notes: [], done: false };

export const sleep = ms => new Promise(r => setTimeout(r, ms));
export const FIXED_CLOCK = Date.UTC(2001, 9, 23, 9, 41, 0);   // deterministic guest clocks

// ---------- helpers (exported for hooks) ----------
export async function waitFor(pred, timeout = 10000, step = 20) {
  const t = performance.now();
  while (performance.now() - t < timeout) { if (await pred()) return performance.now() - t; await sleep(step); }
  throw new Error(`timed out after ${timeout} ms`);
}
/**
 * Resolves once the screen has not changed by more than `tolerance` pixels for `quiet` ms. The tolerance lets a
 * blinking text caret through; cores that report whole-frame damage make frame events useless for this.
 */
export async function settle(m, quiet = 1500, timeout = 30000, tolerance = 50) {
  let prev = await m.screen.read(), since = performance.now();
  const t0 = performance.now();
  while (performance.now() - since < quiet) {
    if (performance.now() - t0 > timeout) throw new Error(`screen did not settle in ${timeout} ms`);
    await sleep(100);
    const cur = await m.screen.read();
    if (cur.width !== prev.width || diffCount(cur, prev) > tolerance) since = performance.now();
    prev = cur;
  }
}
export function px(img, x, y) { const i = (y * img.width + x) * 4; return [img.data[i], img.data[i + 1], img.data[i + 2]]; }
export function diffCount(a, b) { let n = 0; for (let i = 0; i < a.data.length; i += 4) if (a.data[i] !== b.data[i] || a.data[i + 1] !== b.data[i + 1] || a.data[i + 2] !== b.data[i + 2]) n++; return n; }
export function diffBox(a, b) {
  let x0 = Infinity, y0 = Infinity, x1 = -1, y1 = -1;
  for (let y = 0; y < a.height; y++) for (let x = 0; x < a.width; x++) {
    const i = (y * a.width + x) * 4;
    if (a.data[i] !== b.data[i] || a.data[i + 1] !== b.data[i + 1] || a.data[i + 2] !== b.data[i + 2]) {
      if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y;
    }
  }
  return x1 < 0 ? null : { x: x0, y: y0, width: x1 - x0 + 1, height: y1 - y0 + 1 };
}
/** Wait until region `rect` differs from `before` by more than `min` pixels. */
export async function waitChange(m, before, rect, min = 50, timeout = 3000) {
  return waitFor(async () => diffCount(await m.screen.read(rect), before) > min, timeout, 10);
}
export async function click(m, x, y, hold = 80) {
  m.input.pointer.moveTo(x, y); await sleep(60);
  m.input.pointer.button(0, true); await sleep(hold); m.input.pointer.button(0, false); await sleep(60);
}

const canvas = document.getElementById("shot");
export async function shot(m, name) {
  const img = await m.screen.read();
  canvas.width = img.width; canvas.height = img.height;
  canvas.getContext("2d").putImageData(img, 0, 0);
  const blob = await new Promise(r => canvas.toBlob(r));
  await fetch(`/uploads/${ID}/${name}.png`, { method: "PUT", body: blob });
}
async function putFile(name, blob) { await fetch(`/uploads/${ID}/${name}`, { method: "PUT", body: blob }); }
async function getImage(url) {
  const r = await fetch(url); if (!r.ok) return null;
  const bmp = await createImageBitmap(await r.blob());
  const c = new OffscreenCanvas(bmp.width, bmp.height).getContext("2d"); c.drawImage(bmp, 0, 0);
  return c.getImageData(0, 0, bmp.width, bmp.height);
}

// ---------- suites ----------
const profile = await loadProfile(`/profiles/${ID}.json`);
const hooks = SUITE === "smoke" ? null : (await import(`./hooks/${ID}.mjs`)).default;
const result = (name, pass, detail = {}) => { window.RESULTS.tests[name] = { pass, ...detail }; log(`${name} ${pass ? "PASS" : "FAIL"} ${JSON.stringify(detail)}`); };
const want = name => !ONLY || ONLY.has(name);

async function smoke() {
  const t0 = performance.now();
  const m = await Machine.create({ profile, clock: Q.get("realclock") ? undefined : { start: FIXED_CLOCK } });
  m.on("log", t => log("  core:", t));
  m.on("resize", r => log("  resize", r.width, r.height));
  let frames = 0; m.on("frame", () => frames++);
  log(`started in ${(performance.now() - t0).toFixed(0)} ms`);
  for (let i = 1; i <= 12; i++) { await sleep(5000); log(`t=${i * 5}s frames=${frames} screen=${m.screen.width}x${m.screen.height}`); await shot(m, `smoke-${String(i * 5).padStart(2, "0")}s`); }
  const acc = await m.disks.get(profile.disks[0].id).access();
  log(`disk: ${acc.chunkFetches} chunks, ${(acc.bytesFetched / 1048576).toFixed(1)} MB, ${acc.fetchMs.toFixed(0)} ms in sync fetches`);
  await fetch(`/uploads/${ID}/touched.json`, { method: "PUT", body: JSON.stringify(acc.touched) });
  result("smoke", true, { frames, disk: { chunks: acc.chunkFetches, fetch_ms: Math.round(acc.fetchMs) } });
  await m.destroy();
}

// Bake: boot, let the guest finish first-boot reconfiguration, shut it down cleanly, export the disk overlay
// (merge it into the base image with tools/apply-overlay.mjs).
async function bake() {
  const m = await Machine.create({ profile, clock: { start: FIXED_CLOCK } });
  m.on("log", t => log("  core:", t));
  await sleep(+(Q.get("bakewait") || 60000));
  await shot(m, "bake-before-shutdown");
  await hooks.shutdown(m, { sleep, settle, click });
  await sleep(20000);
  await shot(m, "bake-after-shutdown");
  const ov = await m.disks.get(profile.disks[0].id).exportOverlay();
  await fetch(`/uploads/${ID}/bake-overlay.bin`, { method: "PUT", body: ov });
  result("bake", true, { overlay_kb: Math.round(ov.size / 1024) });
  await m.destroy();
}

// Explore: boot, then run "steps" from the URL, e.g. steps=down:185,8;shot:special;up:185,8
async function explore() {
  const m = await Machine.create({ profile, clock: { start: FIXED_CLOCK } });
  await waitFor(async () => hooks.ready(await m.screen.read()), hooks.bootTimeout ?? 120000, 100);
  await settle(m, hooks.settle ?? 2000, 60000);
  for (const step of (Q.get("steps") || "").split(";").filter(Boolean)) {
    const [op, arg = ""] = step.split(":"); const [a, b] = arg.split(",").map(Number);
    if (op === "move") m.input.pointer.moveTo(a, b);
    if (op === "down") { m.input.pointer.moveTo(a, b); await sleep(60); m.input.pointer.button(0, true); }
    if (op === "up") { m.input.pointer.moveTo(a, b); await sleep(60); m.input.pointer.button(0, false); }
    if (op === "shot") await shot(m, "explore-" + arg);
    if (op === "wait") await sleep(a);
    await sleep(400);
  }
  result("explore", true);
  await m.destroy();
}

// Pointer responsiveness: restore the desktop snapshot, then (a) sweep the pointer at 60 Hz for 3 s and count frames,
// (b) jump 20 times and time until the cursor shows up at the target.
async function latency() {
  const snapUrl = `/uploads/${ID}/s0.webusnap`;
  const m = await Machine.create({ profile, snapshot: { url: snapUrl } });
  await waitFor(() => m.screen.seq > 0, 5000, 5);
  await sleep(1500);
  const c = hooks.cursor, a = c.area ?? { x: 100, y: 100, width: 300, height: 200 };
  let frames = 0; const off = m.on("frame", () => frames++);
  const t0 = performance.now();
  while (performance.now() - t0 < 3000) {
    const t = (performance.now() - t0) / 1000;
    m.input.pointer.moveTo(a.x + a.width / 2 + Math.cos(t * 4) * a.width / 3, a.y + a.height / 2 + Math.sin(t * 4) * a.height / 3);
    await new Promise(r => requestAnimationFrame(r));
  }
  off();
  const sweepFps = frames / 3;
  const lat = [];
  for (let i = 0; i < 20; i++) {
    const x = a.x + 30 + (i % 5) * (a.width - 60) / 4, y = a.y + 30 + Math.floor(i / 5) * (a.height - 60) / 3;
    const win = { x: Math.round(x) - 20, y: Math.round(y) - 20, width: 44, height: 44 };
    m.input.pointer.moveTo(c.park[0], c.park[1]); await sleep(150);
    const base = await m.screen.read(win);
    const ts = performance.now();
    m.input.pointer.moveTo(x, y);
    try { await waitChange(m, base, win, 3, 2000); lat.push(+(performance.now() - ts).toFixed(1)); } catch { lat.push(null); }
  }
  const ok = lat.filter(x => x !== null).sort((p, q) => p - q);
  result("latency", true, { sweep_fps: +sweepFps.toFixed(1), jump_ms_median: ok[ok.length >> 1], jump_ms_p90: ok[Math.floor(ok.length * 0.9)], jump_ms: lat });
  await m.destroy();
}

async function conformance() {
  const H = hooks;
  const bootTimeout = H.bootTimeout ?? 120000;
  const cold = () => Machine.create({ profile, clock: { start: FIXED_CLOCK } });
  const ready = async m => {
    if (Q.get("nopoll")) await sleep(+Q.get("nopoll"));   // diagnostic: do not touch the screen while booting
    await waitFor(async () => H.ready(await m.screen.read()), bootTimeout, 100); await settle(m, H.settle ?? 2000, 60000);
  };

  // T1 · cold boot
  let t = performance.now();
  const m = await cold();
  m.on("log", s => { if (!/^(PRAM|WARNING)/.test(s)) log("  core:", s); });
  try { await ready(m); result("T1", true, { boot_s: +((performance.now() - t) / 1000).toFixed(2) }); }
  catch (e) {
    // Diagnostics: is the guest still reading the disk, and are frames still coming?
    const dbg = []; const offLog = m.on("log", t => { if (t.startsWith("debug ")) dbg.push(t.slice(6)); });
    for (let k = 0; k < 3; k++) { m._debug?.(); await sleep(500); }
    offLog(); log("T1 debug:", dbg.join(" | "));
    const acc = await m.disks.get(profile.disks[0].id).access();
    let frames = 0; const off = m.on("frame", () => frames++); await sleep(3000); off();
    const a2 = await m.disks.get(profile.disks[0].id).access();
    result("T1", false, { error: e.message, disk_chunks: acc.chunkFetches, disk_chunks_3s_later: a2.chunkFetches, frames_in_3s: frames, screen: `${m.screen.width}x${m.screen.height}` });
    await shot(m, "T1-fail"); await m.destroy(); return;
  }
  await shot(m, "T1-ready");

  // Snapshot of the settled desktop, used by T2–T4, T7.
  t = performance.now();
  const s0 = await m.saveState();
  const tSave = performance.now();
  const s0frame = await m.screen.read();
  window.RESULTS.notes.push({ snapshot_ms: +(tSave - t).toFixed(1), snapshot_mb: +(s0.size / 1048576).toFixed(2) });
  log(`snapshot ${(tSave - t).toFixed(0)} ms, ${(s0.size / 1048576).toFixed(2)} MB`);
  await putFile("s0.webusnap", s0);

  // T6 · exact pointer (on a restored copy, so the desktop snapshot stays untouched). The cursor's position is found
  // by diffing against a cursor-free frame over a solid area; `offset` is where the diff box starts relative to the
  // hot spot for that cursor on that background.
  if (want("T6")) {
    const c = H.cursor, errs = [];
    try {
      const r = await Machine.create({ profile, snapshot: s0 });
      await waitFor(() => r.screen.seq > 0, 5000, 5);
      const area = c.prepare ? await c.prepare(r, { waitChange, click, sleep, settle, shot: n => shot(r, n) }) : c.area;
      const park = () => r.input.pointer.moveTo(c.park[0], c.park[1]);
      park(); await sleep(300); await settle(r, 400);
      await shot(r, "T6-area");
      let seed = 7; const rnd = n => (seed = (seed * 1103515245 + 12345) % 2147483648) % n;
      for (let i = 0; i < 20; i++) {
        const x = area.x + 24 + rnd(area.width - 48), y = area.y + 24 + rnd(area.height - 48);
        const win = { x: x - 20, y: y - 20, width: 44, height: 44 };
        park(); await sleep(120); await settle(r, 250);
        const base = await r.screen.read(win);
        r.input.pointer.moveTo(x, y);
        await waitChange(r, base, win, 3, 3000); await settle(r, 250);
        const box = diffBox(base, await r.screen.read(win));
        errs.push(box ? [win.x + box.x - c.offset[0] - x, win.y + box.y - c.offset[1] - y] : null);
      }
      await r.destroy();
      result("T6", errs.every(e => e && e[0] === 0 && e[1] === 0), { errors: errs.map(e => e ? e.join(",") : "none") });
    } catch (e) { result("T6", false, { error: e.message, errors: errs }); }
  }

  // T5 · pausing freezes the clock
  if (want("T5")) {
    try {
      let frames = 0; const off = m.on("frame", () => frames++);
      const g0 = m.clock.now(); await m.pause(); const g1 = m.clock.now();
      const f0 = frames; await sleep(10000); const g2 = m.clock.now(); const fPaused = frames - f0;
      await m.resume(); await sleep(1000); const g3 = m.clock.now(); off();
      const detail = { pause_latency_ms: +(g1 - g0).toFixed(1), paused_advance_ms: +(g2 - g1).toFixed(1), after_1s_run_ms: +(g3 - g2).toFixed(1), frames_while_paused: fPaused, state_after: m.state };
      result("T5", g2 - g1 < 100 && g3 - g2 < 1500 && fPaused === 0 && m.state === "running", detail);
    } catch (e) { result("T5", false, { error: e.message }); }
  }

  // T8 · disk overlay round trip: shut the guest down cleanly, export what it wrote, boot a new machine with it
  if (want("T8")) {
    try {
      if (H.shutdown) {                       // a clean shutdown where the core supports guest power-off
        await H.shutdown(m, { waitChange, click, sleep, settle });
        await sleep(H.shutdownWait ?? 10000);
        await shot(m, "T8-shutdown");
      }
      const ov = await m.disks.get(profile.disks[0].id).exportOverlay();
      const m2 = await Machine.create({ profile, clock: { start: FIXED_CLOCK }, disks: [{ ...profile.disks[0], overlay: ov }] });
      if (H.afterUncleanBoot && !H.shutdown) await H.afterUncleanBoot(m2, { waitChange, click, sleep, settle, shot: n => shot(m2, n) });
      try { await ready(m2); }
      catch (e) { await shot(m2, "T8-fail"); m2._debug(); await sleep(500); throw e; }
      const ov2 = await m2.disks.get(profile.disks[0].id).exportOverlay();
      await m2.destroy();
      result("T8", ov.size > 8 && ov2.size >= ov.size, { overlay_kb: +(ov.size / 1024).toFixed(1), after_reboot_kb: +(ov2.size / 1024).toFixed(1) });
    } catch (e) { result("T8", false, { error: e.message }); }
  }
  await m.destroy();

  // T2 · snapshot round trip into fresh instances (×5), T3 · alive after restore
  let baseline = null;
  if (want("T2") || want("T3")) {
    const diffs = [], restoreMs = [];
    try {
      for (let i = 0; i < 5; i++) {
        t = performance.now();
        const r = await Machine.create({ profile, snapshot: s0 });
        await waitFor(() => r.screen.seq > 0, 5000, 5);
        restoreMs.push(+(performance.now() - t).toFixed(1));
        diffs.push(diffCount(await r.screen.read(), s0frame));
        if (i === 4) {
          try { baseline = await H.alive(r, { waitChange, click, sleep, settle, shot: n => shot(r, n) }); result("T3", true, baseline); }
          catch (e) { result("T3", false, { error: e.message }); await shot(r, "T3-fail"); }
          await shot(r, "T3-after");
        }
        await r.destroy();
      }
      result("T2", diffs.every(d => d === 0), { first_frame_diff_px: diffs, restore_ms: restoreMs });
    } catch (e) { result("T2", false, { error: e.message, first_frame_diff_px: diffs }); }
  }

  // T7 · keyboard: type every printable ASCII character, compare with the golden image (T10: nothing lost)
  if (want("T7")) {
    try {
      const r = await Machine.create({ profile, snapshot: s0 });
      const rect = await H.textEcho.open(r, { waitChange, click, sleep, settle, shot: n => shot(r, n) });
      const text = H.textEcho.text ?? Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i)).join("");
      await r.input.type(text);
      await settle(r, 800, 20000);
      // Ignore pixels that change by themselves (a blinking caret).
      const reads = []; for (let i = 0; i < 6; i++) { reads.push(await r.screen.read(rect)); await sleep(250); }
      const unstable = new Uint8Array(rect.width * rect.height);
      for (const img of reads.slice(1)) for (let p = 0; p < unstable.length; p++) if (img.data[p * 4] !== reads[0].data[p * 4]) unstable[p] = 1;
      await shot(r, "T7-typed");
      const goldenUrl = `/conformance/golden/${ID}-T7.png`;
      let golden = RECORD ? null : await getImage(goldenUrl);
      if (!golden) {
        const c = new OffscreenCanvas(rect.width, rect.height); c.getContext("2d").putImageData(reads[0], 0, 0);
        await fetch(`/uploads/golden/${ID}-T7.png`, { method: "PUT", body: await c.convertToBlob() });
        result("T7", false, { recorded: `uploads/golden/${ID}-T7.png`, note: "golden recorded; review it and copy to conformance/golden/" });
      } else {
        let d = 0; for (let p = 0; p < unstable.length; p++) if (!unstable[p] && golden.data[p * 4] !== reads[0].data[p * 4]) d++;
        result("T7", d === 0, { diff_px: d, unstable_px: unstable.reduce((a, b) => a + b, 0), chars: text.length });
      }
      await r.destroy();
    } catch (e) { result("T7", false, { error: e.message }); }
  }

  // T9 · snapshot while busy (during boot), then restore and finish booting
  if (want("T9")) {
    try {
      const b = await cold();
      await sleep(H.busyAt ?? 1500);
      t = performance.now();
      const s = await b.saveState({ timeout: 2000 });
      const ms = performance.now() - t;
      await b.destroy();
      const r = await Machine.create({ profile, snapshot: s });
      await ready(r);
      await r.destroy();
      result("T9", ms <= 2000, { snapshot_ms: +ms.toFixed(1) });
    } catch (e) { result("T9", false, { error: e.message }); }
  }

  // T4 · a snapshot restored 60 s after saving responds like a fresh one
  if (want("T4")) {
    try {
      const wait = 60000 - (performance.now() - tSave);
      if (wait > 0) { log(`T4: waiting ${(wait / 1000).toFixed(0)} s`); await sleep(wait); }
      const r = await Machine.create({ profile, snapshot: s0 });
      await waitFor(() => r.screen.seq > 0, 5000, 5);
      const timings = await H.alive(r, { waitChange, click, sleep, settle, shot: n => shot(r, n) });
      await r.destroy();
      const ok = baseline ? Object.keys(timings).every(k => timings[k] <= Math.max(2 * baseline[k], baseline[k] + 50)) : true;
      result("T4", ok, { gap_s: +((performance.now() - tSave) / 1000).toFixed(0), timings, baseline });
    } catch (e) { result("T4", false, { error: e.message }); }
  }

  // T12 · disk fetches do not stall the guest. Restore with every disk fetch delayed by 400 ms (and a cold cache),
  // keep the pointer moving at 60 Hz, and run the profile's diskLoad (default: open the T7 app), which reads the disk. A guest that blocks on a fetch
  // stops drawing the moving cursor for ≥ 400 ms; one that keeps running draws it every frame or two.
  // The sweep steps aside while the hook uses the pointer (it may be dragging through a menu), and only gaps that
  // fall entirely inside sweeping time count.
  if (want("T12")) {
    try {
      const LATENCY = 400;
      const r = await Machine.create({ profile, snapshot: s0, debug: { diskLatencyMs: LATENCY } });
      await waitFor(() => r.screen.seq > 0, 5000, 5);
      const disk = r.disks.get(profile.disks[0].id), fetched0 = (await disk.access()).chunkFetches;
      const a = hooks.cursor.area ?? { x: 100, y: 100, width: 300, height: 200 };
      const moveTo = r.input.pointer.moveTo, button = r.input.pointer.button;
      let hookUntil = 0, held = 0;
      // Waiting for the screen to change or settle needs a still pointer: hold the sweep meanwhile.
      const still = f => async (...args) => { held++; try { return await f(...args); } finally { held--; hookUntil = performance.now() + 300; } };
      r.input.pointer.moveTo = (x, y) => { hookUntil = performance.now() + 300; moveTo(x, y); };
      r.input.pointer.button = (i, down) => { held += down ? 1 : -1; hookUntil = performance.now() + 300; button(i, down); };
      const frames = [], sweeping = []; let sweepFrom = null, running = true;
      const off = r.on("frame", () => frames.push(performance.now()));
      const sweep = (async () => {
        const t0 = performance.now();
        while (running) {
          const now = performance.now(), on = held === 0 && now >= hookUntil;
          if (on && sweepFrom === null) sweepFrom = now;
          if (!on && sweepFrom !== null) { sweeping.push([sweepFrom, now]); sweepFrom = null; }
          if (on) { const t = (now - t0) / 1000; moveTo(a.x + a.width / 2 + Math.cos(t * 4) * a.width / 3, a.y + a.height / 2 + Math.sin(t * 4) * a.height / 3); }
          await new Promise(res => requestAnimationFrame(res));
        }
        if (sweepFrom !== null) sweeping.push([sweepFrom, performance.now()]);
      })();
      await sleep(1000);
      await (H.diskLoad ?? H.textEcho.open)(r, { waitChange: still(waitChange), click, sleep, settle: still(settle), shot: n => shot(r, n) });
      await sleep(1000);
      running = false; await sweep; off();
      r.input.pointer.moveTo = moveTo; r.input.pointer.button = button;
      const fetched = (await disk.access()).chunkFetches - fetched0;
      const inSweep = (s, e) => sweeping.some(([a0, a1]) => s >= a0 && e <= a1);
      const gaps = [];
      for (let i = 1; i < frames.length; i++) if (inSweep(frames[i - 1], frames[i])) gaps.push(frames[i] - frames[i - 1]);
      gaps.sort((p, q) => p - q);
      const max = gaps.length ? gaps[gaps.length - 1] : Infinity, sweptMs = sweeping.reduce((s, [a0, a1]) => s + a1 - a0, 0);
      await shot(r, "T12-after");
      result("T12", fetched >= 3 && max < LATENCY / 2, {
        latency_ms: LATENCY, fetches: fetched, swept_ms: Math.round(sweptMs), frames: gaps.length + 1,
        gap_ms_p50: +(gaps[gaps.length >> 1] ?? 0).toFixed(0), gap_ms_p99: +(gaps[Math.floor(gaps.length * 0.99)] ?? 0).toFixed(0), gap_ms_max: +max.toFixed(0),
      });
      await r.destroy();
    } catch (e) { result("T12", false, { error: e.message }); }
  }

  // T11 · everything above ran headless (no canvas attached)
  const tt = window.RESULTS.tests;
  if (tt.T7) result("T10", tt.T7.pass, { note: "T7 sends 95 characters (~380 key events) back to back; the text must come out complete and in order" });
  result("T11", !!(tt.T1?.pass && tt.T2?.pass && tt.T3?.pass), { note: "T1–T3 ran with no display attached" });
}

try { if (SUITE === "smoke") await smoke(); else if (SUITE === "bake") await bake(); else if (SUITE === "explore") await explore(); else if (SUITE === "latency") await latency(); else await conformance(); }
catch (e) { log("harness error", e.stack || e); window.RESULTS.error = String(e); }
window.RESULTS.done = true; log("done");
