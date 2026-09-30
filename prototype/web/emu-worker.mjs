// One host for Infinite Mac's emulator builds (Mini vMac, Basilisk II): our own workerApi, plus snapshot/restore via
// Asyncify. The page supplies the module URL, command line, files and disks. Control goes through a SharedArrayBuffer
// because the emulator loop blocks this worker.
const C = { PENDING: 0, MOUSE_FLAG: 1, X: 2, Y: 3, BUTTON: 4, KEY_FLAG: 5, KEYCODE: 6, KEYSTATE: 7,
  SPEED_FLAG: 8, SPEED: 9, SNAPSHOT: 10, WAKE: 11, ZERO: 12 };
const CHUNK = 4096;
let THROTTLE = 100;
let ctrl, M, disks = [], video = { w: 0, h: 0, ptr: 0 }, dirty = false, lastPost = 0;
// Virtual clock: the emulator only reads time through Date.now / performance.now (Emscripten's clock imports call
// them). A snapshot records the clock; a restore offsets it so the guest sees no time pass while it was not running.
const realDate = Date.now.bind(Date), realPerf = performance.now.bind(performance);
let offDate = 0, offPerf = 0;
Date.now = () => realDate() + offDate;
performance.now = () => realPerf() + offPerf;
const post = (m, t) => self.postMessage(m, t || []);
const log = (...a) => post({ type: "log", text: a.join(" ") });
const snapApi = () => (M && M.snap) || {};
// The CPU loop can re-enter itself when native code calls back into guest code (Basilisk II: Execute68k; SheepShaver:
// execute_macos_code), and SheepShaver can also poll input from its idle path (execute_sheep -> idle_wait). With
// Asyncify limited to the main path, frames on those other paths cannot be unwound. So only snapshot when the stack
// holds the CPU loop exactly once and none of the other frames; otherwise try again at the next input poll.
Error.stackTraceLimit = 500;
let deferred = 0, guard = null;             // set per emulator by the page; Mini vMac needs none
function nestedCpu() {
  if (!guard) return false;
  const st = new Error().stack;
  const bad = (st.match(guard.cpu) || []).length !== 1 || (guard.forbid && guard.forbid.test(st));
  if (bad) deferred++;
  return bad;
}

function postFrame(force) {
  const now = performance.now();
  if (!video.ptr || (!force && (!dirty || now - lastPost < THROTTLE))) return;
  const px = M.HEAPU8.slice(video.ptr, video.ptr + video.w * video.h * 4);
  dirty = false; lastPost = now;
  post({ type: "frame", w: video.w, h: video.h, px, t: now }, [px.buffer]);
}

function takeSnapshot(A) {
  const t0 = performance.now(), snap = snapApi(), ex = snap.exports();
  const id = new Int32Array(M.HEAPU8.buffer)[(A.currData + 8) >> 2];
  const rewindName = Object.keys(ex).find(k => ex[k] === A.funcWrappers.get(A.callStackIdToFunc.get(id)));
  const s = {
    mem: M.HEAPU8.slice(0), sp: snap.stackSave(), currData: A.currData, rewindId: id, rewindName,
    disks: disks.map(d => { const chunks = [...d.dirty].sort((a, b) => a - b);
      const data = new Uint8Array(chunks.length * CHUNK);
      chunks.forEach((c, i) => data.set(d.bytes.subarray(c * CHUNK, (c + 1) * CHUNK), i * CHUNK));
      return { name: d.name, chunks, data }; }),
    video: { ...video }, clock: { date: Date.now(), perf: performance.now() },
  };
  s.copy_ms = performance.now() - t0;
  post({ type: "snapshot", s }, [s.mem.buffer, ...s.disks.map(d => d.data.buffer)]);
}

self.workerApi = {
  InputBufferAddresses: { mouseButtonStateAddr: C.BUTTON, mousePositionFlagAddr: C.MOUSE_FLAG, mousePositionXAddr: C.X,
    mousePositionYAddr: C.Y, keyEventFlagAddr: C.KEY_FLAG, keyCodeAddr: C.KEYCODE, keyStateAddr: C.KEYSTATE,
    speedFlagAddr: C.SPEED_FLAG, speedAddr: C.SPEED, mouseDeltaXAddr: C.ZERO, mouseDeltaYAddr: C.ZERO,
    useMouseDeltasFlagAddr: C.ZERO, useMouseDeltasAddr: C.ZERO, ethernetInterruptFlagAddr: C.ZERO },
  acquireInputLock() {
    const A = snapApi().asyncify;
    if (A && A.state === 2) return A.handleSleep(() => {});   // rewinding (restore or wake-up) ends here
    postFrame(false);
    if (A && Atomics.load(ctrl, C.SNAPSHOT) === 1 && !nestedCpu()) {
      Atomics.store(ctrl, C.SNAPSHOT, 0);
      const tUnwind = performance.now();
      return A.handleSleep(wakeUp => {
        setTimeout(() => {                       // the wasm stack is empty now: everything is in linear memory
          log(`unwound in ${(performance.now() - tUnwind).toFixed(1)} ms (deferred ${deferred}x while nested)`);
          takeSnapshot(A);
          wakeUp(0);
        }, 0);
      });
    }
    return Atomics.load(ctrl, C.PENDING);
  },
  getInputValue: addr => Atomics.load(ctrl, addr),
  releaseInputLock() {
    Atomics.store(ctrl, C.MOUSE_FLAG, 0); Atomics.store(ctrl, C.BUTTON, -1); Atomics.store(ctrl, C.KEY_FLAG, 0);
    Atomics.store(ctrl, C.SPEED_FLAG, 0); Atomics.store(ctrl, C.PENDING, 0);
  },
  sleep(sec) { postFrame(false); Atomics.wait(ctrl, C.WAKE, 0, sec * 1000); },
  idleWait() { postFrame(false); Atomics.wait(ctrl, C.WAKE, 0, 8); return Atomics.load(ctrl, C.PENDING); },
  didOpenVideo(w, h) { video.w = w; video.h = h; log(`video ${w}x${h}`); },
  blit(ptr) { if (ptr) { video.ptr = ptr; dirty = true; } postFrame(false); },
  didOpenAudio() {}, enqueueAudio() { return 0; }, audioBufferSize() { return 1 << 20; },
  getClipboardText: () => "", setClipboardText() {},
  etherSeed: () => 1, etherInit() {}, etherWrite() {}, etherRead: () => 0,
  disks: {
    open(name) { return disks.findIndex(d => d.name === name); },
    close() {},
    size: id => disks[id].bytes.length,
    read(id, ptr, off, len) { M.HEAPU8.set(disks[id].bytes.subarray(off, off + len), ptr); return len; },
    write(id, ptr, off, len) {
      const d = disks[id]; d.bytes.set(M.HEAPU8.subarray(ptr, ptr + len), off);
      for (let c = Math.floor(off / CHUNK); c <= Math.floor((off + len - 1) / CHUNK); c++) d.dirty.add(c);
      return len;
    },
    consumeDiskName: () => null,
    isMediaPresent: () => 1, isFixedDisk: () => 1, eject() {},
  },
};

self.addEventListener("unhandledrejection", e => log("unhandled", e.reason && (e.reason.stack || e.reason)));
self.onmessage = async ({ data }) => {
  ctrl = new Int32Array(data.ctrl);
  if (data.throttle != null) THROTTLE = data.throttle;
  if (data.guard) guard = { cpu: new RegExp(data.guard.cpu, "g"), forbid: data.guard.forbid ? new RegExp(data.guard.forbid) : null };
  const t0 = realPerf();
  const factory = (await import(data.module)).default;
  const restore = data.snapshot;
  // Base disk image (shared, cacheable) plus, on restore, the chunks this machine had written.
  disks = [{ name: data.diskName, bytes: new Uint8Array(data.disk), dirty: new Set() }];
  if (restore) restore.disks.forEach((o, i) => o.chunks.forEach((c, j) => {
    disks[i].bytes.set(o.data.subarray(j * CHUNK, (j + 1) * CHUNK), c * CHUNK); disks[i].dirty.add(c); }));
  // The factory's promise never settles on a cold boot (main() blocks this thread), so keep our own reference to
  // the Module object; the --pre-js has filled in M.snap before main() starts.
  M = {
    arguments: data.args || [], noInitialRun: !!restore,
    print: t => log("[emu]", t), printErr: t => log("[emu!]", t),
    preRun: [mod => {
      for (const [path, content] of Object.entries(data.files))
        mod.FS.writeFile(path, typeof content === "string" ? content : new Uint8Array(content));
      post({ type: "ready", t_ms: performance.now() - t0 });
    }],
  };
  try { await factory(M); } catch (e) { log("factory error", e && (e.stack || e)); return; }
  if (!restore) return;

  // Restore into this fresh instance: memory, stack pointer, Asyncify bookkeeping, then rewind into main().
  const snap = snapApi(), A = snap.asyncify, s = restore;
  const tr = performance.now();
  if (s.clock && !data.noClockFix) { offDate = s.clock.date - realDate(); offPerf = s.clock.perf - realPerf(); }
  M.HEAPU8.set(new Uint8Array(s.mem));
  snap.stackRestore(s.sp);
  video = { ...s.video };
  const orig = [...A.funcWrappers].find(([, w]) => w === snap.exports()[s.rewindName])[0];
  A.callStackIdToFunc.set(s.rewindId, orig); A.callstackFuncToId.set(orig, s.rewindId);
  A.callStackId = Math.max(A.callStackId, s.rewindId + 1);
  A.currData = s.currData;
  dirty = true; postFrame(true);                 // first frame straight from restored memory
  post({ type: "restored", t_ms: realPerf() - t0, memset_ms: realPerf() - tr });
  A.state = 2;
  snap.startRewind(A.currData);
  A.doRewind(A.currData);                        // does not return while the emulator runs
};
