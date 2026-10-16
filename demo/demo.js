import { Machine, loadProfile } from "./dist/webulator.js";

// OpenArena 0.8.8 playing back a recorded match (loops), in a 640x480 window, with its frame counter on.
const OPENARENA = { name: "OpenArena", cmd: "cmd /c cd /d C:\\OpenArena && openarena.exe +set s_initsound 0 +set r_fullscreen 0 +set r_mode 3 " +
  "+set cg_drawFPS 1 +set com_introplayed 1 +set timedemo 0 +set d1 \"demo demo088-test1; set nextdemo vstr d1\" +vstr d1" };
const MACHINES = [
  { arch: "x86", id: "v86-win98", os: "Windows 98", hw: "PC", core: "v86", year: 1998 },
  // Same profile as plain XP. The saved state is the settled desktop; the page then starts OpenArena in the guest, because
  // the host-side 3D state (WebGL textures, buffers) is not part of a snapshot, so a state saved mid-game cannot resume.
  { arch: "x86", id: "v86-winxp-3d", profile: "v86-winxp", snapshot: "v86-winxp-3d", os: "Windows XP", hw: "PC", core: "v86", year: 2001,
    badge: "3D", tag: "x86 · v86 · 3D accelerated", needs3d: true, launch: OPENARENA },
  { arch: "68k", id: "minivmac-plus-system608", os: "System 6.0.8", hw: "Macintosh Plus", core: "Mini vMac", year: 1991 },
  { arch: "68k", id: "basilisk2-quadra650-system753", os: "System 7.5.3", hw: "Quadra 650", core: "Basilisk II", year: 1996 },
  { arch: "PowerPC", id: "sheepshaver-g3bw-macos904", os: "Mac OS 9.0.4", hw: "Power Macintosh G3", core: "SheepShaver", year: 2000 },
];
const DEFAULT = "basilisk2-quadra650-system753";

const $ = id => document.getElementById(id);
// The 3D machine renders with WebGL2 inside the emulator's worker (OffscreenCanvas).
function hasWebGL2() { try { return !!new OffscreenCanvas(1, 1).getContext("webgl2"); } catch { return false; } }
const canvas = $("screen"), veil = $("veil");
let current = null, machine = null, saved = null, busy = false, pending = null;

function status(text, state = "") { $("statusText").textContent = text; $("status").className = "status " + state; }
function controls() {
  const live = machine && machine.state !== "destroyed" && machine.state !== "crashed";
  $("wake").disabled = busy || !current; $("boot").disabled = busy || !current;
  $("pause").disabled = busy || !live; $("save").disabled = busy || !live; $("restore").disabled = busy || !live || !saved;
  $("pause").textContent = machine?.state === "paused" ? "Resume" : "Pause";
}

// The machine tabs.
const nav = $("machines");
for (const m of MACHINES) {
  const b = document.createElement("button");
  b.className = "machine"; b.dataset.id = m.id;
  b.innerHTML = `<b>${m.os}${m.badge ? ` <span class="badge">${m.badge}</span>` : ""}</b><small>${m.tag ?? `${m.arch} · ${m.core}`}</small>`;
  b.onclick = () => select(m.id, true);
  nav.append(b);
}

// The call log: each library call this page makes, with its result.
const calls = $("calls");
function call(code) {
  const li = document.createElement("li"), c = document.createElement("span"), r = document.createElement("span");
  c.className = "c"; c.innerHTML = code; r.className = "r"; r.textContent = "…";
  li.append(c, r); calls.append(li); calls.scrollTop = calls.scrollHeight;
  return {
    ok(text) { r.textContent = "→ " + text; r.className = "r ok"; },
    fail(e) { r.textContent = "✕ " + (e?.message ?? e); r.className = "r err"; },
  };
}
const ms = t0 => `${(performance.now() - t0).toFixed(0)} ms`;
const mb = n => `${(n / 1048576).toFixed(1)} MB`;

async function fetchWithProgress(url, label) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const total = +res.headers.get("content-length") || 0, reader = res.body.getReader(), parts = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value); got += value.length;
    status(`${label} ${(got / 1048576).toFixed(1)}${total ? ` / ${(total / 1048576).toFixed(1)}` : ""} MB`);
  }
  return new Blob(parts);
}

async function teardown() {
  if (machine) { const c = call("<em>await</em> m.destroy()"); await machine.destroy(); c.ok("destroyed"); machine = null; }
  saved = null;
}

async function start(fromSnapshot) {
  if (!current) return;
  if (current.needs3d && !hasWebGL2()) { veil.hidden = false; veil.textContent = "This machine needs WebGL2, which this browser does not offer."; status("WebGL2 is not available"); return; }
  if (busy) { pending = fromSnapshot; return; }     // run it once the machine being started is up
  busy = true; controls();
  let c = { fail() {} };
  await teardown();
  veil.hidden = false; veil.textContent = fromSnapshot ? "Waking…" : "Booting…";
  try {
    const pid = current.profile ?? current.id, sid = current.snapshot ?? current.id;
    c = call(`profile = <em>await</em> loadProfile("${pid}.json")`);
    const profile = await loadProfile(`profiles/${pid}.json`);
    c.ok(`${profile.core.adapter} · ${profile.machine.name}`);
    let snapshot;
    if (fromSnapshot) {
      c = call(`snapshot = <em>await</em> fetch("${sid}.webusnap")`);
      snapshot = await fetchWithProgress(`snapshots/${sid}.webusnap`, "Downloading saved state");
      c.ok(mb(snapshot.size));
    }
    status(fromSnapshot ? "Restoring…" : "Starting the emulator…");
    c = call(`m = <em>await</em> Machine.create({ profile${fromSnapshot ? ", snapshot" : ""} })`);
    const t0 = performance.now();
    machine = await Machine.create({ profile, snapshot });
    const took = ms(t0);
    c.ok(`${machine.state} in ${took}`);
    machine.screen.attach(canvas, { input: true });
    call("m.screen.attach(canvas, { input: <em>true</em> })").ok(`${machine.info.screen.width}×${machine.info.screen.height}, ${machine.info.arch}`);
    machine.on("state", s => { status(s === "paused" ? "Paused" : s === "crashed" ? "The emulator stopped" : "Running", s); controls(); });
    veil.hidden = true;
    status(fromSnapshot ? `Running · restored in ${took}` : "Running · booting", "running");
    canvas.focus();
    if (fromSnapshot && current.launch) launch(machine, current.launch);
  } catch (e) {
    veil.hidden = false; veil.textContent = "Could not start this machine.";
    c.fail(e); status(e.message);
  }
  busy = false; controls();
  if (pending !== null) { const p = pending; pending = null; start(p); }
}

// Start a program in the guest the way a user would: Win+R, type the command, Enter. The first pointer move switches the
// guest's mouse driver to absolute mode. Then show how much of the disk the guest has read while the program loads.
async function launch(m, app) {
  const alive = () => machine === m && m.state !== "destroyed" && m.state !== "crashed";
  const sleep = t => new Promise(r => setTimeout(r, t));
  const c = call(`m.input.pointer.moveTo(…); m.input.key("Meta+R"); <em>await</em> m.input.type("${app.name.toLowerCase()}…")`);
  try {
    // A restored guest pages its working set in from the disk first; over the network that can take many seconds, and keys
    // typed meanwhile are lost. Wait until it has stopped reading (two quiet seconds, at most 90 s).
    const disk0 = m.disks.get("hd0"); let last = -1, quiet = 0;
    for (let t = performance.now(); alive() && quiet < 2 && performance.now() - t < 90000; ) {
      await sleep(1000); const f = (await disk0.access()).bytesFetched;
      quiet = f === last ? quiet + 1 : 0; last = f; status(`Running · loading ${app.name}'s desktop, disk read ${mb(f)}`, "running");
    }
    m.input.pointer.moveTo(500, 400); await sleep(300); m.input.pointer.moveTo(520, 420); await sleep(500);
    m.input.key("MetaLeft", true); m.input.key("KeyR", true); m.input.key("KeyR", false); m.input.key("MetaLeft", false);
    await sleep(1500);
    if (!alive()) return;
    await m.input.type(app.cmd + "\n");
    c.ok("typed into the Run dialog");
    const disk = m.disks.get("hd0"), t0 = performance.now();
    while (alive() && performance.now() - t0 < 120000) {
      await sleep(1000);
      if (!alive() || busy || m.state === "paused") continue;
      const a = await disk.access();
      status(`Running · ${app.name}, disk read ${mb(a.bytesFetched)}`, "running");
    }
    if (alive() && !busy && m.state !== "paused") status("Running", "running");
  } catch (e) { c.fail(e); }
}

function select(id, wake) {
  if (current?.id !== id) calls.replaceChildren();   // the log shows the calls for the machine on screen
  current = MACHINES.find(m => m.id === id);
  for (const b of nav.querySelectorAll(".machine")) b.setAttribute("aria-current", String(b.dataset.id === id));
  history.replaceState(null, "", `#${id}`);
  $("note3d").hidden = !current.needs3d;
  controls();
  if (wake) start(true);
}

$("wake").onclick = () => start(true);
$("boot").onclick = () => start(false);
$("pause").onclick = async () => {
  if (!machine) return;
  busy = true; controls();
  const resume = machine.state === "paused", c = call(`<em>await</em> m.${resume ? "resume" : "pause"}()`), t0 = performance.now();
  if (resume) await machine.resume(); else await machine.pause();
  c.ok(`${machine.state} in ${ms(t0)}`);
  busy = false; controls();
};
$("save").onclick = async () => {
  busy = true; controls(); status("Saving…");
  const c = call("saved = <em>await</em> m.saveState()");
  try {
    const t0 = performance.now();
    saved = await machine.saveState();
    c.ok(`${mb(saved.size)} in ${ms(t0)}`);
    status(`Running · saved ${mb(saved.size)}`, "running");
  } catch (e) { c.fail(e); status(e.message); }
  busy = false; controls();
};
$("restore").onclick = async () => {
  busy = true; controls(); status("Restoring…");
  const c = call("<em>await</em> m.restoreState(saved)");
  try {
    const t0 = performance.now();
    await machine.restoreState(saved);
    c.ok(`restored in ${ms(t0)}`);
    status(`Running · restored in ${ms(t0)}`, "running");
    canvas.focus();
  } catch (e) { c.fail(e); status(e.message); }
  busy = false; controls();
};

if (!window.crossOriginIsolated) {
  status("Setting up cross-origin isolation…");
} else {
  const fromHash = MACHINES.find(m => m.id === location.hash.slice(1));
  select((fromHash ?? MACHINES.find(m => m.id === DEFAULT)).id, true);
}

$("try3d").onclick = () => { select("v86-winxp-3d", true); document.querySelector(".demo").scrollIntoView({ behavior: "smooth", block: "start" }); };
