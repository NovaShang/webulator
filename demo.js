import { Machine, loadProfile } from "./dist/webulator.js";

const MACHINES = [
  { arch: "x86", id: "v86-win98", os: "Windows 98", hw: "PC", core: "v86", year: 1998 },
  { arch: "68k", id: "minivmac-plus-system608", os: "System 6.0.8", hw: "Macintosh Plus", core: "Mini vMac", year: 1991 },
  { arch: "68k", id: "basilisk2-quadra650-system753", os: "System 7.5.3", hw: "Quadra 650", core: "Basilisk II", year: 1996 },
  { arch: "PowerPC", id: "sheepshaver-g3bw-macos904", os: "Mac OS 9.0.4", hw: "Power Macintosh G3", core: "SheepShaver", year: 2000 },
];
const DEFAULT = "basilisk2-quadra650-system753";

const $ = id => document.getElementById(id);
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
  b.innerHTML = `<b>${m.os}</b><small>${m.arch} · ${m.core}</small>`;
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
  if (busy) { pending = fromSnapshot; return; }     // run it once the machine being started is up
  busy = true; controls();
  let c = { fail() {} };
  await teardown();
  veil.hidden = false; veil.textContent = fromSnapshot ? "Waking…" : "Booting…";
  try {
    c = call(`profile = <em>await</em> loadProfile("${current.id}.json")`);
    const profile = await loadProfile(`profiles/${current.id}.json`);
    c.ok(`${profile.core.adapter} · ${profile.machine.name}`);
    let snapshot;
    if (fromSnapshot) {
      c = call(`snapshot = <em>await</em> fetch("${current.id}.webusnap")`);
      snapshot = await fetchWithProgress(`snapshots/${current.id}.webusnap`, "Downloading saved state");
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
  } catch (e) {
    veil.hidden = false; veil.textContent = "Could not start this machine.";
    c.fail(e); status(e.message);
  }
  busy = false; controls();
  if (pending !== null) { const p = pending; pending = null; start(p); }
}

function select(id, wake) {
  if (current?.id !== id) calls.replaceChildren();   // the log shows the calls for the machine on screen
  current = MACHINES.find(m => m.id === id);
  for (const b of nav.querySelectorAll(".machine")) b.setAttribute("aria-current", String(b.dataset.id === id));
  history.replaceState(null, "", `#${id}`);
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
