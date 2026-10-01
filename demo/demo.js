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
let current = null, machine = null, saved = null, busy = false;

function status(text, state = "") { $("statusText").textContent = text; $("status").className = "status " + state; }
function controls() {
  const live = machine && machine.state !== "destroyed" && machine.state !== "crashed";
  $("wake").disabled = busy || !current; $("boot").disabled = busy || !current;
  $("pause").disabled = busy || !live; $("save").disabled = busy || !live; $("restore").disabled = busy || !live || !saved;
  $("pause").textContent = machine?.state === "paused" ? "Resume" : "Pause";
}

// The machine list, grouped by architecture.
const nav = $("machines");
for (const arch of [...new Set(MACHINES.map(m => m.arch))]) {
  const h = document.createElement("div"); h.className = "arch"; h.textContent = arch; nav.append(h);
  for (const m of MACHINES.filter(x => x.arch === arch)) {
    const b = document.createElement("button");
    b.className = "machine"; b.dataset.id = m.id;
    b.innerHTML = `<b>${m.os}</b><span class="year">${m.year}</span><small>${m.hw} · ${m.core}</small>`;
    b.onclick = () => select(m.id, true);
    nav.append(b);
  }
}

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
  if (machine) { await machine.destroy(); machine = null; }
  saved = null;
}

async function start(fromSnapshot) {
  if (!current || busy) return;
  busy = true; controls();
  await teardown();
  veil.hidden = false; veil.textContent = fromSnapshot ? "Waking…" : "Booting…";
  try {
    const profile = await loadProfile(`profiles/${current.id}.json`);
    let snapshot;
    if (fromSnapshot) snapshot = await fetchWithProgress(`snapshots/${current.id}.webusnap`, "Downloading saved state");
    status(fromSnapshot ? "Restoring…" : "Starting the emulator…");
    const t0 = performance.now();
    machine = await Machine.create({ profile, snapshot });
    const ms = performance.now() - t0;
    machine.screen.attach(canvas, { input: true });
    machine.on("state", s => { status(s === "paused" ? "Paused" : s === "crashed" ? "The emulator stopped" : "Running", s); controls(); });
    veil.hidden = true;
    status(fromSnapshot ? `Running · restored in ${ms.toFixed(0)} ms` : "Running · booting", "running");
    canvas.focus();
  } catch (e) {
    veil.hidden = false; veil.textContent = "Could not start this machine.";
    status(e.message);
  }
  busy = false; controls();
}

function select(id, wake) {
  current = MACHINES.find(m => m.id === id);
  for (const b of nav.querySelectorAll(".machine")) b.setAttribute("aria-current", String(b.dataset.id === id));
  $("title").textContent = `${current.os} on ${current.hw}`;
  $("meta").textContent = `${current.arch} · ${current.core}`;
  history.replaceState(null, "", `#${id}`);
  controls();
  if (wake) start(true);
}

$("wake").onclick = () => start(true);
$("boot").onclick = () => start(false);
$("pause").onclick = async () => {
  if (!machine) return;
  busy = true; controls();
  if (machine.state === "paused") await machine.resume(); else await machine.pause();
  busy = false; controls();
};
$("save").onclick = async () => {
  busy = true; controls(); status("Saving…");
  try {
    const t0 = performance.now();
    saved = await machine.saveState();
    status(`Running · saved ${(saved.size / 1048576).toFixed(1)} MB in ${(performance.now() - t0).toFixed(0)} ms`, "running");
  } catch (e) { status(e.message); }
  busy = false; controls();
};
$("restore").onclick = async () => {
  busy = true; controls(); status("Restoring…");
  try {
    const t0 = performance.now();
    await machine.restoreState(saved);
    status(`Running · restored in ${(performance.now() - t0).toFixed(0)} ms`, "running");
    canvas.focus();
  } catch (e) { status(e.message); }
  busy = false; controls();
};

if (!window.crossOriginIsolated) {
  status("Setting up cross-origin isolation…");
} else {
  const fromHash = MACHINES.find(m => m.id === location.hash.slice(1));
  select((fromHash ?? MACHINES.find(m => m.id === DEFAULT)).id, true);
}
