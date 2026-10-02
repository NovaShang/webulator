// src/worker/clock.ts
var realDate = Date.now.bind(Date);
var realPerf = performance.now.bind(performance);
var offDate = realDate() - realPerf();
var offPerf = 0;
var frozenAt = null;
var nowReal = () => frozenAt ?? realPerf();
var guestDate = () => nowReal() + offDate;
var guestPerf = () => nowReal() + offPerf;
Date.now = guestDate;
performance.now = guestPerf;
var clock = {
  start(epochMs) {
    offDate = epochMs - realPerf();
  },
  pause() {
    if (frozenAt === null) frozenAt = realPerf();
  },
  resume() {
    if (frozenAt === null) return;
    const gap = realPerf() - frozenAt;
    offDate -= gap;
    offPerf -= gap;
    frozenAt = null;
  },
  save() {
    return { date: guestDate(), perf: guestPerf() };
  },
  restore(c) {
    const r = realPerf();
    offDate = c.date - r;
    offPerf = c.perf - r;
    frozenAt = null;
  }
};

// src/control.ts
var CTRL = {
  FLAGS: 0,
  // requests from the main thread, see FLAG
  WAKE: 1,
  // bumped + notified on every input, wakes idle waits
  HEAD: 2,
  // input ring write index (main thread)
  TAIL: 3,
  // input ring read index (worker)
  PX: 4,
  // latest absolute pointer position (coalesced, not in the ring)
  PY: 5,
  PSEQ: 6,
  // bumped whenever PX/PY change
  PAUSED: 7,
  // worker sets 1 while paused
  RING: 16
};
var FLAG = {
  PAUSE: 1,
  SNAPSHOT: 2,
  OVERLAYS: 4,
  // export disk overlays
  DEBUG: 8
  // diagnostics: the adapter logs internal state once
};
var RING_SIZE = 256;
var RING_STRIDE = 4;
var CTRL_INTS = CTRL.RING + RING_SIZE * RING_STRIDE;
var EV = {
  KEY: 1,
  // a = key index (KEY_CODES), b = down
  BUTTON: 2,
  // a = button index, b = down
  MOVE: 3,
  // a = dx, b = dy
  WHEEL: 4
  // a = dx, b = dy
};
function peekEvent(v) {
  const tail = Atomics.load(v, CTRL.TAIL);
  if (tail === Atomics.load(v, CTRL.HEAD)) return null;
  const i = CTRL.RING + tail % RING_SIZE * RING_STRIDE;
  return { type: v[i], a: v[i + 1], b: v[i + 2], c: v[i + 3] };
}
function consumeEvent(v) {
  Atomics.add(v, CTRL.TAIL, 1);
}

// src/worker/disks.ts
var OVERLAY_BLOCK = 4096;
function fetchSync({ url, range }) {
  const x = new XMLHttpRequest();
  x.open("GET", url, false);
  x.responseType = "arraybuffer";
  if (range) x.setRequestHeader("Range", `bytes=${range[0]}-${range[1]}`);
  x.send();
  if (x.status !== 200 && x.status !== 206) throw new Error(`disk read ${url}: HTTP ${x.status}`);
  return new Uint8Array(x.response);
}
var SLOTS = 8;
var SLOT_SIZE = 1 << 20;
var FREE = 0;
var BUSY = 1;
var DONE = 2;
var FAILED = 3;
var FETCHER_SRC = `
let st, lens, data, ctrl, latency;
onmessage = async ({ data: m }) => {
  if (m.init) { st = new Int32Array(m.init.st); lens = new Int32Array(m.init.lens); data = m.init.data;
    ctrl = new Int32Array(m.init.ctrl); latency = m.init.latency; return; }
  let state = ${FAILED};
  try {
    if (latency) await new Promise(r => setTimeout(r, latency));
    const res = await fetch(m.url, m.range ? { headers: { Range: "bytes=" + m.range[0] + "-" + m.range[1] } } : undefined);
    if (res.ok) {
      const b = new Uint8Array(await res.arrayBuffer());
      if (b.length <= ${SLOT_SIZE}) { new Uint8Array(data, m.slot * ${SLOT_SIZE}, b.length).set(b); lens[m.slot] = b.length; state = ${DONE}; }
    }
  } catch {}
  Atomics.store(st, m.slot, state);
  Atomics.add(ctrl, ${CTRL.WAKE}, 1); Atomics.notify(ctrl, ${CTRL.WAKE});
};`;
var Fetcher = class {
  st = new Int32Array(new SharedArrayBuffer(SLOTS * 4));
  lens = new Int32Array(new SharedArrayBuffer(SLOTS * 4));
  data = new SharedArrayBuffer(SLOTS * SLOT_SIZE);
  jobs = new Array(SLOTS).fill(null);
  queue = [];
  worker;
  constructor(ctrl, latency) {
    this.worker = new Worker(URL.createObjectURL(new Blob([FETCHER_SRC], { type: "text/javascript" })));
    this.worker.postMessage({ init: { st: this.st.buffer, lens: this.lens.buffer, data: this.data, ctrl, latency } });
  }
  request(job) {
    this.queue.push(job);
    this.dispatch();
  }
  dispatch() {
    for (let s = 0; s < SLOTS && this.queue.length; s++) {
      if (this.jobs[s]) continue;
      const job = this.queue.shift();
      this.jobs[s] = job;
      Atomics.store(this.st, s, BUSY);
      this.worker.postMessage({ slot: s, url: job.src.url, range: job.src.range });
    }
  }
  /** Hand finished chunks to their devices. Cheap when nothing is in flight. */
  pump() {
    let freed = false;
    for (let s = 0; s < SLOTS; s++) {
      const job = this.jobs[s];
      if (!job) continue;
      const state = Atomics.load(this.st, s);
      if (state === DONE) job.dev.deliver(job.i, new Uint8Array(this.data, s * SLOT_SIZE, this.lens[s]).slice());
      else if (state === FAILED) job.dev.deliver(job.i, void 0);
      else continue;
      this.jobs[s] = null;
      Atomics.store(this.st, s, FREE);
      freed = true;
    }
    if (freed) this.dispatch();
  }
};
var fetcher = null;
var testLatency = 0;
function startFetcher(ctrl, latencyMs = 0) {
  testLatency = latencyMs;
  fetcher ??= new Fetcher(ctrl, latencyMs);
}
function pumpFetches() {
  fetcher?.pump();
}
var BlockDevice = class _BlockDevice {
  id;
  size;
  readOnly;
  base;
  cache;
  inflight = /* @__PURE__ */ new Set();
  failed = /* @__PURE__ */ new Set();
  // background fetch failed: the next read fetches synchronously
  overlay = /* @__PURE__ */ new Map();
  stats = { chunkFetches: 0, bytesFetched: 0, fetchMs: 0, syncFetches: 0 };
  /** Base chunks in the order they were first needed; a prefetch list can be made from it. */
  touched = [];
  constructor(id, size, readOnly, base) {
    this.id = id;
    this.size = size;
    this.readOnly = readOnly;
    this.base = base;
    this.cache = new Array(base.count);
  }
  static async open(d) {
    const s = d.source;
    let dev;
    if (s.buffer instanceof ArrayBuffer) {
      const buf = new Uint8Array(s.buffer);
      dev = new _BlockDevice(d.id, buf.length, d.readOnly, { chunkSize: buf.length, count: 1, source: () => null });
      dev.cache[0] = buf;
    } else if (typeof s.manifest === "string") {
      const res = await fetch(s.manifest);
      if (!res.ok) throw new Error(`disk manifest ${s.manifest}: HTTP ${res.status}`);
      const m = await res.json();
      const baseUrl = new URL(m.baseUrl ?? "chunks/", s.manifest).href;
      dev = new _BlockDevice(d.id, m.size, d.readOnly, {
        chunkSize: m.chunkSize,
        count: m.chunks.length,
        source: (i) => m.chunks[i] ? { url: baseUrl + m.chunks[i] } : null
      });
      await Promise.all((m.prefetch ?? []).filter((i) => m.chunks[i]).map(async (i) => {
        const r = await fetch(baseUrl + m.chunks[i]);
        dev.cache[i] = new Uint8Array(await r.arrayBuffer());
      }));
    } else if (typeof s.url === "string" && typeof s.size === "number") {
      const url = s.url, size = s.size, cs = 256 * 1024;
      dev = new _BlockDevice(d.id, size, d.readOnly, {
        chunkSize: cs,
        count: Math.ceil(size / cs),
        source: (i) => ({ url, range: [i * cs, Math.min(size, (i + 1) * cs) - 1] })
      });
    } else {
      throw new Error(`disk ${d.id}: unknown source`);
    }
    if (d.overlay) dev.importOverlay(d.overlay);
    return dev;
  }
  chunk(i) {
    let c = this.cache[i];
    if (c === void 0) {
      const src = this.base.source(i), t = performance.now();
      if (src && testLatency) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, testLatency);
      c = src ? fetchSync(src) : null;
      this.cache[i] = c;
      this.failed.delete(i);
      this.touched.push(i);
      if (c) {
        this.stats.chunkFetches++;
        this.stats.syncFetches++;
        this.stats.bytesFetched += c.length;
        this.stats.fetchMs += performance.now() - t;
      }
    }
    return c;
  }
  fetchStarted = /* @__PURE__ */ new Map();
  /** A background fetch finished (data) or failed (undefined). */
  deliver(i, data) {
    this.inflight.delete(i);
    if (this.cache[i] !== void 0) return;
    if (!data) {
      this.failed.add(i);
      return;
    }
    this.cache[i] = data;
    this.touched.push(i);
    this.stats.chunkFetches++;
    this.stats.bytesFetched += data.length;
    this.stats.fetchMs += performance.now() - (this.fetchStarted.get(i) ?? performance.now());
  }
  /**
   * Whether [off, off + len) can be read or written now without a network wait. If not, the missing chunks are
   * requested in the background; ask again later. Overlay blocks that a range covers need no base data.
   */
  ready(off, len) {
    pumpFetches();
    const cs = this.base.chunkSize, end = Math.min(this.size, off + len);
    let ok = true, prev = -1;
    for (let b = Math.floor(off / OVERLAY_BLOCK); b * OVERLAY_BLOCK < end; b++) {
      if (this.overlay.has(b)) continue;
      const first = Math.floor(b * OVERLAY_BLOCK / cs), last = Math.floor((Math.min(end, (b + 1) * OVERLAY_BLOCK) - 1) / cs);
      for (let i = Math.max(first, prev + 1); i <= last; i++) {
        if (!this.has(i)) ok = false;
        prev = i;
      }
    }
    return ok;
  }
  /** Chunk i is cached, or will be read synchronously (no fetcher, or its fetch failed). Starts a fetch if not. */
  has(i) {
    if (this.cache[i] !== void 0 || this.failed.has(i) || !fetcher) return true;
    const src = this.base.source(i);
    if (!src) {
      this.cache[i] = null;
      return true;
    }
    if (!this.inflight.has(i)) {
      this.inflight.add(i);
      this.fetchStarted.set(i, performance.now());
      fetcher.request({ dev: this, i, src });
    }
    return false;
  }
  /** Copy base-image bytes (no overlay) into dst. */
  readBase(off, len, dst, at) {
    const cs = this.base.chunkSize;
    while (len > 0) {
      const i = Math.floor(off / cs), o = off - i * cs, n = Math.min(len, cs - o);
      const c = this.chunk(i);
      if (c) dst.set(c.subarray(o, o + n), at);
      else dst.fill(0, at, at + n);
      off += n;
      at += n;
      len -= n;
    }
  }
  read(off, len, dst, at = 0) {
    if (this.overlay.size === 0) return this.readBase(off, len, dst, at);
    while (len > 0) {
      const b = Math.floor(off / OVERLAY_BLOCK), o = off - b * OVERLAY_BLOCK, n = Math.min(len, OVERLAY_BLOCK - o);
      const blk = this.overlay.get(b);
      if (blk) dst.set(blk.subarray(o, o + n), at);
      else this.readBase(off, n, dst, at);
      off += n;
      at += n;
      len -= n;
    }
  }
  write(off, src) {
    let at = 0, len = src.length;
    while (len > 0) {
      const b = Math.floor(off / OVERLAY_BLOCK), o = off - b * OVERLAY_BLOCK, n = Math.min(len, OVERLAY_BLOCK - o);
      let blk = this.overlay.get(b);
      if (!blk) {
        blk = new Uint8Array(OVERLAY_BLOCK);
        if (n < OVERLAY_BLOCK) this.readBase(b * OVERLAY_BLOCK, Math.min(OVERLAY_BLOCK, this.size - b * OVERLAY_BLOCK), blk, 0);
        this.overlay.set(b, blk);
      }
      blk.set(src.subarray(at, at + n), o);
      off += n;
      at += n;
      len -= n;
    }
  }
  exportOverlay() {
    const chunks = [...this.overlay.keys()].sort((a, b) => a - b);
    const data = new Uint8Array(chunks.length * OVERLAY_BLOCK);
    chunks.forEach((c, i) => data.set(this.overlay.get(c), i * OVERLAY_BLOCK));
    return { chunkSize: OVERLAY_BLOCK, chunks, data: data.buffer };
  }
  importOverlay(o) {
    if (o.chunkSize !== OVERLAY_BLOCK) throw new Error(`overlay block size ${o.chunkSize} unsupported`);
    const data = new Uint8Array(o.data);
    o.chunks.forEach((c, i) => this.overlay.set(c, data.slice(i * OVERLAY_BLOCK, (i + 1) * OVERLAY_BLOCK)));
  }
};

// src/worker/env.ts
var FRAME_INTERVAL = 12;
var CLOCK_INTERVAL = 250;
var Env = class {
  ctrl;
  init;
  disks;
  files;
  paused = false;
  // pointer state kept in guest pixels
  px = 0;
  py = 0;
  pseq = 0;
  // video
  width = 0;
  height = 0;
  src = null;
  fixAlpha = false;
  dirty = null;
  seq = 0;
  lastPost = 0;
  lastClock = 0;
  constructor(init, disks, files) {
    this.init = init;
    this.ctrl = new Int32Array(init.ctrl);
    this.disks = disks;
    this.files = files;
    this.width = init.profile.machine.screen.width;
    this.height = init.profile.machine.screen.height;
  }
  post(m, transfer = []) {
    self.postMessage(m, transfer);
  }
  log(text) {
    this.post({ type: "log", text });
  }
  // ---------- lifecycle ----------
  started() {
    this.post({ type: "started", width: this.width, height: this.height, guest: Date.now() });
  }
  crash(message) {
    if (!this.halted) this.post({ type: "crash", message });
  }
  flag(bit) {
    return (Atomics.load(this.ctrl, CTRL.FLAGS) & bit) !== 0;
  }
  clearFlag(bit) {
    Atomics.and(this.ctrl, CTRL.FLAGS, ~bit);
  }
  /**
   * Called by the adapter at every input poll. Handles pause (blocking adapters wait here), overlay export,
   * frame and clock posting. Returns whether a snapshot was requested; the adapter takes it at a safe point.
   */
  poll(canBlock, allowSnapshot = true) {
    this.flush(false);
    pumpFetches();
    if (this.flag(FLAG.OVERLAYS)) {
      this.clearFlag(FLAG.OVERLAYS);
      const overlays = this.exportOverlays();
      const access = Object.fromEntries([...this.disks].map(([id, d]) => [id, { ...d.stats, touched: d.touched }]));
      this.post({ type: "overlays", overlays, access }, Object.values(overlays).map((o) => o.data));
    }
    if (this.flag(FLAG.PAUSE) && !this.paused) this.enterPause();
    if (this.paused && canBlock) {
      while (this.flag(FLAG.PAUSE)) {
        if (allowSnapshot && this.flag(FLAG.SNAPSHOT)) return { snapshot: true };
        if (this.flag(FLAG.OVERLAYS)) return this.poll(canBlock, allowSnapshot);
        const w = Atomics.load(this.ctrl, CTRL.WAKE);
        Atomics.wait(this.ctrl, CTRL.WAKE, w, 500);
      }
    }
    if (!this.flag(FLAG.PAUSE) && this.paused) this.leavePause();
    return { snapshot: this.flag(FLAG.SNAPSHOT) };
  }
  /** The core's main loop has ended (the guest powered off). Keep serving requests from a timer. */
  halted = false;
  halt() {
    if (this.halted) return;
    this.halted = true;
    this.log("core stopped (guest powered off)");
    this.flush(true);
    setInterval(() => {
      if (this.flag(FLAG.SNAPSHOT)) {
        this.clearFlag(FLAG.SNAPSHOT);
        this.log("snapshot requested after power-off; ignored");
      }
      this.poll(false, false);
    }, 20);
  }
  enterPause() {
    this.paused = true;
    clock.pause();
    Atomics.store(this.ctrl, CTRL.PAUSED, 1);
    this.flush(true);
    this.post({ type: "state", state: "paused", guest: Date.now() });
  }
  leavePause() {
    this.paused = false;
    clock.resume();
    Atomics.store(this.ctrl, CTRL.PAUSED, 0);
    this.post({ type: "state", state: "running", guest: Date.now() });
  }
  /** Block until input arrives or `ms` pass (used by cores' idle waits). */
  idle(ms) {
    const w = Atomics.load(this.ctrl, CTRL.WAKE);
    if (this.hasInput()) return;
    Atomics.wait(this.ctrl, CTRL.WAKE, w, ms);
  }
  // ---------- input ----------
  hasInput() {
    return Atomics.load(this.ctrl, CTRL.PSEQ) !== this.pseq || Atomics.load(this.ctrl, CTRL.HEAD) !== Atomics.load(this.ctrl, CTRL.TAIL);
  }
  /**
   * Take pending input: pointer moves (absolute, coalesced; relative moves are folded in) and at most one discrete
   * event (key, button, wheel), so cores that handle one event per poll never drop any.
   */
  takeInput(holdDiscrete = false) {
    let moved = false;
    const s = Atomics.load(this.ctrl, CTRL.PSEQ);
    if (s !== this.pseq) {
      this.pseq = s;
      this.px = Atomics.load(this.ctrl, CTRL.PX);
      this.py = Atomics.load(this.ctrl, CTRL.PY);
      moved = true;
    }
    for (; ; ) {
      const e = peekEvent(this.ctrl);
      if (!e) return { moved, discrete: null };
      if (e.type === EV.MOVE) {
        this.px = Math.max(0, Math.min(this.width - 1, this.px + e.a));
        this.py = Math.max(0, Math.min(this.height - 1, this.py + e.b));
        moved = true;
        consumeEvent(this.ctrl);
        continue;
      }
      if (holdDiscrete) return { moved, discrete: null };
      consumeEvent(this.ctrl);
      if (e.type === EV.KEY) return { moved, discrete: { kind: "key", code: e.a, down: e.b !== 0 } };
      if (e.type === EV.BUTTON) return { moved, discrete: { kind: "button", index: e.a, down: e.b !== 0 } };
      if (e.type === EV.WHEEL) return { moved, discrete: { kind: "wheel", dx: e.a, dy: e.b } };
    }
  }
  // ---------- video ----------
  /** The adapter's current RGBA framebuffer (a view; may live in wasm memory). */
  setSource(src, width, height, fixAlpha = false) {
    const resized = width !== this.width || height !== this.height;
    this.src = src;
    this.width = width;
    this.height = height;
    this.fixAlpha = fixAlpha;
    if (resized) this.dirty = { x: 0, y: 0, width, height };
  }
  /** Mark part of the frame as changed (default: all of it). Posting is throttled. */
  damage(r) {
    const full = { x: 0, y: 0, width: this.width, height: this.height };
    const d = r ?? full;
    if (!this.dirty) this.dirty = { ...d };
    else {
      const x0 = Math.min(this.dirty.x, d.x), y0 = Math.min(this.dirty.y, d.y);
      const x1 = Math.max(this.dirty.x + this.dirty.width, d.x + d.width), y1 = Math.max(this.dirty.y + this.dirty.height, d.y + d.height);
      this.dirty = { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
    }
  }
  copyFrame() {
    const n = this.width * this.height * 4;
    const out = this.src ? this.src.slice(0, n) : new Uint8Array(n);
    if (this.fixAlpha) for (let i = 3; i < n; i += 4) out[i] = 255;
    return out;
  }
  flush(force) {
    const now = realPerf();
    if (this.dirty && this.src && (force || now - this.lastPost >= FRAME_INTERVAL)) {
      const rgba = this.copyFrame();
      const dirty = [this.dirty];
      this.dirty = null;
      this.lastPost = now;
      this.lastClock = now;
      this.post({ type: "frame", seq: ++this.seq, width: this.width, height: this.height, rgba: rgba.buffer, dirty, guest: Date.now() }, [rgba.buffer]);
    } else if (now - this.lastClock >= CLOCK_INTERVAL) {
      this.lastClock = now;
      this.post({ type: "clock", guest: Date.now() });
    }
  }
  // ---------- disks & snapshots ----------
  exportOverlays() {
    return Object.fromEntries([...this.disks].map(([id, d]) => [id, d.exportOverlay()]));
  }
  /** Called by the adapter once it holds its core state. Clears the request. */
  sendSnapshot(core) {
    const overlays = this.exportOverlays();
    const rgba = this.copyFrame();
    this.clearFlag(FLAG.SNAPSHOT);
    this.post(
      { type: "snapshot", core, overlays, clock: clock.save(), frame: { width: this.width, height: this.height, rgba: rgba.buffer } },
      [core, rgba.buffer, ...Object.values(overlays).map((o) => o.data)]
    );
  }
};

// src/keyboard.ts
var KEY_CODES = [
  "KeyA",
  "KeyB",
  "KeyC",
  "KeyD",
  "KeyE",
  "KeyF",
  "KeyG",
  "KeyH",
  "KeyI",
  "KeyJ",
  "KeyK",
  "KeyL",
  "KeyM",
  "KeyN",
  "KeyO",
  "KeyP",
  "KeyQ",
  "KeyR",
  "KeyS",
  "KeyT",
  "KeyU",
  "KeyV",
  "KeyW",
  "KeyX",
  "KeyY",
  "KeyZ",
  "Digit0",
  "Digit1",
  "Digit2",
  "Digit3",
  "Digit4",
  "Digit5",
  "Digit6",
  "Digit7",
  "Digit8",
  "Digit9",
  "Minus",
  "Equal",
  "BracketLeft",
  "BracketRight",
  "Backslash",
  "Semicolon",
  "Quote",
  "Backquote",
  "Comma",
  "Period",
  "Slash",
  "Enter",
  "Tab",
  "Space",
  "Backspace",
  "Escape",
  "CapsLock",
  "ShiftLeft",
  "ShiftRight",
  "ControlLeft",
  "ControlRight",
  "AltLeft",
  "AltRight",
  "MetaLeft",
  "MetaRight",
  "ContextMenu",
  "ArrowLeft",
  "ArrowRight",
  "ArrowUp",
  "ArrowDown",
  "Home",
  "End",
  "PageUp",
  "PageDown",
  "Insert",
  "Delete",
  "F1",
  "F2",
  "F3",
  "F4",
  "F5",
  "F6",
  "F7",
  "F8",
  "F9",
  "F10",
  "F11",
  "F12",
  "F13",
  "F14",
  "F15",
  "NumLock",
  "ScrollLock",
  "PrintScreen",
  "Pause",
  "Numpad0",
  "Numpad1",
  "Numpad2",
  "Numpad3",
  "Numpad4",
  "Numpad5",
  "Numpad6",
  "Numpad7",
  "Numpad8",
  "Numpad9",
  "NumpadDecimal",
  "NumpadEnter",
  "NumpadAdd",
  "NumpadSubtract",
  "NumpadMultiply",
  "NumpadDivide",
  "NumpadEqual"
];
var KEY_INDEX = new Map(KEY_CODES.map((c, i) => [c, i]));
var ADB = {
  KeyA: 0,
  KeyS: 1,
  KeyD: 2,
  KeyF: 3,
  KeyH: 4,
  KeyG: 5,
  KeyZ: 6,
  KeyX: 7,
  KeyC: 8,
  KeyV: 9,
  KeyB: 11,
  KeyQ: 12,
  KeyW: 13,
  KeyE: 14,
  KeyR: 15,
  KeyY: 16,
  KeyT: 17,
  Digit1: 18,
  Digit2: 19,
  Digit3: 20,
  Digit4: 21,
  Digit6: 22,
  Digit5: 23,
  Equal: 24,
  Digit9: 25,
  Digit7: 26,
  Minus: 27,
  Digit8: 28,
  Digit0: 29,
  BracketRight: 30,
  KeyO: 31,
  KeyU: 32,
  BracketLeft: 33,
  KeyI: 34,
  KeyP: 35,
  Enter: 36,
  KeyL: 37,
  KeyJ: 38,
  Quote: 39,
  KeyK: 40,
  Semicolon: 41,
  Backslash: 42,
  Comma: 43,
  Slash: 44,
  KeyN: 45,
  KeyM: 46,
  Period: 47,
  Tab: 48,
  Space: 49,
  Backquote: 50,
  Backspace: 51,
  Escape: 53,
  MetaLeft: 55,
  MetaRight: 55,
  ShiftLeft: 56,
  CapsLock: 57,
  AltLeft: 58,
  ControlLeft: 54,
  ShiftRight: 123,
  AltRight: 124,
  ControlRight: 125,
  ArrowLeft: 59,
  ArrowRight: 60,
  ArrowDown: 61,
  ArrowUp: 62,
  NumpadDecimal: 65,
  NumpadMultiply: 67,
  NumpadAdd: 69,
  NumLock: 71,
  NumpadDivide: 75,
  NumpadEnter: 76,
  NumpadSubtract: 78,
  NumpadEqual: 81,
  Numpad0: 82,
  Numpad1: 83,
  Numpad2: 84,
  Numpad3: 85,
  Numpad4: 86,
  Numpad5: 87,
  Numpad6: 88,
  Numpad7: 89,
  Numpad8: 91,
  Numpad9: 92,
  F1: 122,
  F2: 120,
  F3: 99,
  F4: 118,
  F5: 96,
  F6: 97,
  F7: 98,
  F8: 100,
  F9: 101,
  F10: 109,
  F11: 103,
  F12: 111,
  F13: 105,
  F14: 107,
  F15: 113,
  Home: 115,
  PageUp: 116,
  Delete: 117,
  End: 119,
  PageDown: 121,
  Insert: 114
};
var MINIVMAC = {
  ...ADB,
  ArrowLeft: 123,
  ArrowRight: 124,
  ArrowDown: 125,
  ArrowUp: 126,
  ShiftRight: 56,
  AltRight: 58,
  ControlLeft: 59,
  ControlRight: 59
};
var PS2_SET1 = {
  Escape: 1,
  Digit1: 2,
  Digit2: 3,
  Digit3: 4,
  Digit4: 5,
  Digit5: 6,
  Digit6: 7,
  Digit7: 8,
  Digit8: 9,
  Digit9: 10,
  Digit0: 11,
  Minus: 12,
  Equal: 13,
  Backspace: 14,
  Tab: 15,
  KeyQ: 16,
  KeyW: 17,
  KeyE: 18,
  KeyR: 19,
  KeyT: 20,
  KeyY: 21,
  KeyU: 22,
  KeyI: 23,
  KeyO: 24,
  KeyP: 25,
  BracketLeft: 26,
  BracketRight: 27,
  Enter: 28,
  ControlLeft: 29,
  KeyA: 30,
  KeyS: 31,
  KeyD: 32,
  KeyF: 33,
  KeyG: 34,
  KeyH: 35,
  KeyJ: 36,
  KeyK: 37,
  KeyL: 38,
  Semicolon: 39,
  Quote: 40,
  Backquote: 41,
  ShiftLeft: 42,
  Backslash: 43,
  KeyZ: 44,
  KeyX: 45,
  KeyC: 46,
  KeyV: 47,
  KeyB: 48,
  KeyN: 49,
  KeyM: 50,
  Comma: 51,
  Period: 52,
  Slash: 53,
  ShiftRight: 54,
  NumpadMultiply: 55,
  AltLeft: 56,
  Space: 57,
  CapsLock: 58,
  F1: 59,
  F2: 60,
  F3: 61,
  F4: 62,
  F5: 63,
  F6: 64,
  F7: 65,
  F8: 66,
  F9: 67,
  F10: 68,
  NumLock: 69,
  ScrollLock: 70,
  Numpad7: 71,
  Numpad8: 72,
  Numpad9: 73,
  NumpadSubtract: 74,
  Numpad4: 75,
  Numpad5: 76,
  Numpad6: 77,
  NumpadAdd: 78,
  Numpad1: 79,
  Numpad2: 80,
  Numpad3: 81,
  Numpad0: 82,
  NumpadDecimal: 83,
  F11: 87,
  F12: 88,
  NumpadEnter: 57372,
  ControlRight: 57373,
  NumpadDivide: 57397,
  AltRight: 57400,
  Home: 57415,
  ArrowUp: 57416,
  PageUp: 57417,
  ArrowLeft: 57419,
  ArrowRight: 57421,
  End: 57423,
  ArrowDown: 57424,
  PageDown: 57425,
  Insert: 57426,
  Delete: 57427,
  MetaLeft: 57435,
  MetaRight: 57436,
  ContextMenu: 57437
};

// src/worker/adapters/macemu.ts
var ADDR = { BUTTON: 1, MOUSE_FLAG: 2, X: 3, Y: 4, KEY_FLAG: 5, KEYCODE: 6, KEYSTATE: 7, ZERO: 8 };
var REWINDING = 2;
async function runMacemu(env2) {
  const { profile, memory, restore } = env2.init;
  const cfg = profile.coreConfig;
  const keymap = cfg.keymap === "minivmac" ? MINIVMAC : ADB;
  const W = profile.machine.screen.width, H = profile.machine.screen.height;
  let M;
  let videoPtr = 0, videoW = W, videoH = H;
  const stage = new Int32Array(9);
  let buttonDown = false;
  Error.stackTraceLimit = 500;
  const cpuRe = cfg.guard ? new RegExp(cfg.guard.cpu, "g") : null;
  const forbidRe = cfg.guard?.forbid ? new RegExp(cfg.guard.forbid) : null;
  const atSafePoint = () => {
    if (!cpuRe) return true;
    const st = new Error().stack ?? "";
    return (st.match(cpuRe) ?? []).length === 1 && !(forbidRe && forbidRe.test(st));
  };
  const asyncify = () => M.snap?.asyncify ?? null;
  function captureCore(A2) {
    const snap = M.snap, ex = snap.exports();
    const id = new Int32Array(M.HEAPU8.buffer)[A2.currData + 8 >> 2];
    const rewindName = Object.keys(ex).find((k) => ex[k] === A2.funcWrappers.get(A2.callStackIdToFunc.get(id)));
    const meta2 = new TextEncoder().encode(JSON.stringify({ sp: snap.stackSave(), currData: A2.currData, rewindId: id, rewindName, videoPtr, videoW, videoH }));
    const out = new Uint8Array(4 + meta2.length + M.HEAPU8.length);
    new DataView(out.buffer).setUint32(0, meta2.length, true);
    out.set(meta2, 4);
    out.set(M.HEAPU8, 4 + meta2.length);
    return out.buffer;
  }
  let lastDiscrete = -Infinity, lastBlit = 0;
  function stageInput() {
    stage.fill(0);
    stage[ADDR.BUTTON] = -1;
    const now = performance.now();
    const { moved, discrete } = env2.takeInput(now - lastDiscrete < (cfg.eventIntervalMs ?? 0));
    if (discrete) lastDiscrete = now;
    if (moved) {
      stage[ADDR.MOUSE_FLAG] = 1;
      stage[ADDR.X] = env2.px;
      stage[ADDR.Y] = env2.py;
    }
    if (discrete?.kind === "button" && discrete.index === 0) {
      stage[ADDR.BUTTON] = discrete.down ? 1 : 0;
      buttonDown = discrete.down;
    }
    if (discrete?.kind === "key") {
      const k = keymap[KEY_CODES[discrete.code]];
      if (k !== void 0) {
        stage[ADDR.KEY_FLAG] = 1;
        stage[ADDR.KEYCODE] = k;
        stage[ADDR.KEYSTATE] = discrete.down ? 1 : 0;
      }
    }
    return moved || discrete ? 1 : 0;
  }
  self.workerApi = {
    InputBufferAddresses: {
      mouseButtonStateAddr: ADDR.BUTTON,
      mousePositionFlagAddr: ADDR.MOUSE_FLAG,
      mousePositionXAddr: ADDR.X,
      mousePositionYAddr: ADDR.Y,
      keyEventFlagAddr: ADDR.KEY_FLAG,
      keyCodeAddr: ADDR.KEYCODE,
      keyStateAddr: ADDR.KEYSTATE,
      speedFlagAddr: ADDR.ZERO,
      speedAddr: ADDR.ZERO,
      mouseDeltaXAddr: ADDR.ZERO,
      mouseDeltaYAddr: ADDR.ZERO,
      useMouseDeltasFlagAddr: ADDR.ZERO,
      useMouseDeltasAddr: ADDR.ZERO,
      ethernetInterruptFlagAddr: ADDR.ZERO
    },
    acquireInputLock() {
      const A2 = asyncify();
      if (A2 && A2.state === REWINDING) return A2.handleSleep(() => {
      });
      const { snapshot } = env2.poll(true);
      if (snapshot && A2 && atSafePoint()) {
        return A2.handleSleep((wake) => {
          setTimeout(() => {
            try {
              env2.sendSnapshot(captureCore(A2));
            } catch (e) {
              env2.log(`snapshot failed: ${e}`);
            }
            wake(0);
          }, 0);
        });
      }
      return stageInput();
    },
    getInputValue: (addr) => stage[addr],
    releaseInputLock() {
    },
    sleep(sec) {
      env2.flush(false);
      env2.idle(sec * 1e3);
    },
    // Idle path: honour pause and overlay requests here too (not snapshots: this is not a safe point). Only block
    // until the next ~60 Hz refresh is due: SheepShaver checks its tick (which drives the refresh and the cursor)
    // only every 50k instructions, so long idle blocks starve it (6 fps, 300 ms pointer lag).
    idleWait() {
      env2.poll(true, false);
      const wait = Math.min(8, lastBlit + 16.7 - performance.now() - 1);
      if (wait > 0) env2.idle(wait);
      return env2.hasInput() ? 1 : 0;
    },
    didOpenVideo(w, h) {
      videoW = w;
      videoH = h;
    },
    blit(ptr, _size, rect) {
      lastBlit = performance.now();
      if (ptr) {
        if (ptr !== videoPtr || !env2.width) {
          videoPtr = ptr;
          env2.setSource(M.HEAPU8.subarray(ptr, ptr + videoW * videoH * 4), videoW, videoH, !!cfg.fixAlpha);
        }
        env2.damage(rect ? { x: rect.left, y: rect.top, width: rect.right - rect.left, height: rect.bottom - rect.top } : void 0);
      }
      env2.flush(false);
    },
    didOpenAudio() {
    },
    enqueueAudio() {
      return 0;
    },
    audioBufferSize() {
      return 1 << 20;
    },
    getClipboardText: () => "",
    setClipboardText() {
    },
    etherSeed: () => 1,
    etherInit() {
    },
    etherWrite() {
    },
    etherRead: () => 0,
    disks: {
      open(name) {
        const id = name.replace(/^\*?\/disk\//, "");
        return [...env2.disks.keys()].indexOf(id);
      },
      close() {
      },
      size: (i) => [...env2.disks.values()][i].size,
      read(i, ptr, off, len) {
        [...env2.disks.values()][i].read(off, len, M.HEAPU8, ptr);
        return len;
      },
      // Basilisk II / SheepShaver (patched disk driver): 0 = not fetched yet, the guest retries the request.
      ready: (i, off, len) => [...env2.disks.values()][i].ready(off, len) ? 1 : 0,
      write(i, ptr, off, len) {
        [...env2.disks.values()][i].write(off, M.HEAPU8.subarray(ptr, ptr + len));
        return len;
      },
      consumeDiskName: () => null,
      isMediaPresent: () => 1,
      isFixedDisk: () => 1,
      eject() {
      }
    }
  };
  void buttonDown;
  const prefs = cfg.prefs.replaceAll("${memory}", String(memory)).replaceAll("${width}", String(W)).replaceAll("${height}", String(H)).replaceAll("${disks}", [...env2.disks.keys()].map((id) => `disk /disk/${id}`).join("\n"));
  M = {
    arguments: cfg.args ?? [],
    noInitialRun: !!restore,
    print: (t) => env2.log(t),
    printErr: (t) => env2.log(t),
    onAbort: (what) => env2.crash(String(what)),
    // Guest power-off makes the core call exit(). Mark the machine halted first; the ExitStatus Emscripten then
    // throws (and any trap after it) is swallowed by the worker's error handler, and the worker keeps serving.
    onExit: () => env2.halt(),
    quit: (_status, toThrow) => {
      env2.halt();
      throw toThrow;
    },
    preRun: [(mod) => {
      for (const [path, bytes] of env2.files) mod.FS.writeFile(path, bytes);
      mod.FS.writeFile("/prefs", prefs);
    }]
  };
  const factory = (await import(
    /* @vite-ignore */
    profile.core.module
  )).default;
  if (!restore) {
    env2.started();
    await factory(M);
    return;
  }
  await factory(M);
  const A = asyncify();
  if (!A) throw new Error("this core build has no Asyncify; it cannot restore snapshots");
  const core = new Uint8Array(restore.core);
  const mlen = new DataView(restore.core).getUint32(0, true);
  const meta = JSON.parse(new TextDecoder().decode(core.subarray(4, 4 + mlen)));
  const mem = core.subarray(4 + mlen);
  if (mem.length !== M.HEAPU8.length) throw new Error(`snapshot memory ${mem.length} B, core has ${M.HEAPU8.length} B`);
  M.HEAPU8.set(mem);
  M.snap.stackRestore(meta.sp);
  const orig = [...A.funcWrappers].find(([, w]) => w === M.snap.exports()[meta.rewindName])?.[0];
  if (!orig) throw new Error("snapshot rewind entry not found in this build");
  A.callStackIdToFunc.set(meta.rewindId, orig);
  A.callstackFuncToId.set(orig, meta.rewindId);
  A.callStackId = Math.max(A.callStackId, meta.rewindId + 1);
  A.currData = meta.currData;
  videoPtr = meta.videoPtr;
  videoW = meta.videoW;
  videoH = meta.videoH;
  if (videoPtr) {
    env2.setSource(M.HEAPU8.subarray(videoPtr, videoPtr + videoW * videoH * 4), videoW, videoH, !!cfg.fixAlpha);
    env2.damage();
  }
  env2.flush(true);
  env2.started();
  A.state = REWINDING;
  M.snap.startRewind(A.currData);
  A.doRewind(A.currData);
}

// src/worker/adapters/v86screen.ts
var HeadlessScreen = class {
  width = 720;
  height = 400;
  fb = new Uint8Array(this.width * this.height * 4);
  graphical = false;
  onResize = () => {
  };
  onDamage = () => {
  };
  cols = 80;
  rows = 25;
  chars = new Uint8Array(80 * 25);
  fg = new Int32Array(80 * 25);
  bg = new Int32Array(80 * 25);
  changedRows = new Uint8Array(25);
  fontH = 16;
  fontW = 9;
  font = null;
  copy8th = false;
  cursorRow = 0;
  cursorCol = 0;
  curStart = 14;
  curEnd = 15;
  curOn = true;
  vga = null;
  /** The methods v86's VGA device calls on its screen adapter. */
  methods() {
    return {
      put_char: (row, col, chr, _flags, bgc, fgc) => {
        const i = row * this.cols + col;
        if (i >= this.chars.length) return;
        this.chars[i] = chr;
        this.bg[i] = bgc;
        this.fg[i] = fgc;
        this.changedRows[row] = 1;
      },
      set_mode: (graphical) => {
        this.graphical = graphical;
        if (!graphical) this.changedRows.fill(1);
      },
      set_size_text: (cols, rows) => {
        if (cols === this.cols && rows === this.rows && !this.graphical) return;
        this.cols = cols;
        this.rows = rows;
        this.chars = new Uint8Array(cols * rows);
        this.fg = new Int32Array(cols * rows);
        this.bg = new Int32Array(cols * rows);
        this.changedRows = new Uint8Array(rows).fill(1);
        this.resize(cols * this.fontW, rows * this.fontH);
      },
      set_size_graphical: (w, h) => {
        this.resize(w, h);
      },
      set_font_bitmap: (height, width9, widthDbl, copy8th, bitmap) => {
        const w = widthDbl ? 16 : width9 ? 9 : 8;
        this.font = bitmap;
        this.copy8th = copy8th;
        if (w !== this.fontW || height !== this.fontH) {
          this.fontW = w;
          this.fontH = height;
          if (!this.graphical) this.resize(this.cols * w, this.rows * height);
        }
        this.changedRows.fill(1);
      },
      set_font_page: () => {
        this.changedRows.fill(1);
      },
      clear_screen: () => {
        this.fb.fill(0);
        for (let i = 3; i < this.fb.length; i += 4) this.fb[i] = 255;
        this.onDamage(0, 0, this.width, this.height);
      },
      clear_text_state: () => {
      },
      set_scale: () => {
      },
      update_cursor_scanline: (start, end, _max) => {
        this.curOn = !(start & 32) && start <= end;
        this.curStart = start & 31;
        this.curEnd = end & 31;
        this.changedRows[this.cursorRow] = 1;
      },
      update_cursor: (row, col) => {
        this.changedRows[this.cursorRow] = 1;
        this.cursorRow = row;
        this.cursorCol = col;
        this.changedRows[row] = 1;
      },
      update_buffer: (layers) => {
        for (const l of layers) this.copyLayer(l);
      },
      destroy: () => {
      },
      pause: () => {
      },
      continue: () => {
      },
      get_text_screen: () => [],
      get_text_row: () => ""
    };
  }
  resize(w, h) {
    if (w === this.width && h === this.height) return;
    this.width = w;
    this.height = h;
    this.fb = new Uint8Array(w * h * 4);
    for (let i = 3; i < this.fb.length; i += 4) this.fb[i] = 255;
    this.changedRows.fill(1);
    this.onResize(w, h);
  }
  /**
   * Copy one VGA layer into the frame buffer, clipped on every side. Must never throw: it runs inside v86's
   * screen_fill_buffer, and an exception there skips the vertical-retrace update the guest may be waiting for.
   * v86 re-sends unchanged layers in VGA modes, so only rows that really changed are reported as damage.
   */
  copyLayer(l) {
    const src = l.image_data.data, sw = l.image_data.width, sh = l.image_data.height;
    let bx = l.buffer_x, by = l.buffer_y, dx = l.screen_x, dy = l.screen_y, w = l.buffer_width, h = l.buffer_height;
    if (dx < 0) {
      bx -= dx;
      w += dx;
      dx = 0;
    }
    if (dy < 0) {
      by -= dy;
      h += dy;
      dy = 0;
    }
    if (bx < 0) {
      dx -= bx;
      w += bx;
      bx = 0;
    }
    if (by < 0) {
      dy -= by;
      h += by;
      by = 0;
    }
    w = Math.min(w, this.width - dx, sw - bx);
    h = Math.min(h, this.height - dy, sh - by);
    if (w <= 0 || h <= 0) return;
    let y0 = -1, y1 = -1;
    for (let y = 0; y < h; y++) {
      const s = ((by + y) * sw + bx) * 4, d = ((dy + y) * this.width + dx) * 4, n = w * 4;
      let same = true;
      for (let i = 0; i < n; i += 4) {
        if (src[s + i] !== this.fb[d + i] || src[s + i + 1] !== this.fb[d + i + 1] || src[s + i + 2] !== this.fb[d + i + 2]) {
          same = false;
          break;
        }
      }
      if (same) continue;
      this.fb.set(src.subarray(s, s + n), d);
      if (y0 < 0) y0 = y;
      y1 = y;
    }
    if (y0 >= 0) this.onDamage(dx, dy + y0, w, y1 - y0 + 1);
  }
  /** Render text-mode rows that changed. */
  renderText() {
    if (this.graphical) return;
    let font = this.font;
    if (!font && this.vga) {
      font = this.vga.plane2;
      this.fontH = (this.vga.max_scan_line & 31) + 1 || 16;
    }
    if (!font) return;
    const fw = this.fontW, fh = this.fontH, W = this.width;
    let y0 = -1, y1 = -1;
    for (let r = 0; r < this.rows; r++) {
      if (!this.changedRows[r]) continue;
      this.changedRows[r] = 0;
      if (y0 < 0) y0 = r;
      y1 = r;
      for (let c = 0; c < this.cols; c++) {
        const i = r * this.cols + c, ch = this.chars[i], fgc = this.fg[i], bgc = this.bg[i];
        const cursor = this.curOn && r === this.cursorRow && c === this.cursorCol;
        for (let line = 0; line < fh; line++) {
          const bits = font[ch * 32 + line];
          const curLine = cursor && line >= this.curStart && line <= this.curEnd;
          let p = ((r * fh + line) * W + c * fw) * 4;
          for (let b = 0; b < fw; b++, p += 4) {
            let on = b < 8 ? bits >> 7 - b & 1 : this.copy8th && ch >= 192 && ch <= 223 ? bits & 1 : 0;
            if (curLine) on = 1;
            const col = on ? fgc : bgc;
            this.fb[p] = col >> 16 & 255;
            this.fb[p + 1] = col >> 8 & 255;
            this.fb[p + 2] = col & 255;
            this.fb[p + 3] = 255;
          }
        }
      }
    }
    if (y0 >= 0) this.onDamage(0, y0 * fh, W, (y1 - y0 + 1) * fh);
  }
};

// src/worker/adapters/v86.ts
var waiting = [];
function completeReady() {
  while (waiting.length && waiting[0].dev.ready(waiting[0].start, waiting[0].len)) waiting.shift().run();
}
function diskObject(dev) {
  const read = (start, len) => {
    const d = new Uint8Array(len);
    dev.read(start, len, d);
    return d;
  };
  const op = (start, len, run) => {
    if (!waiting.length && dev.ready(start, len)) run();
    else waiting.push({ dev, start, len, run });
  };
  return {
    byteLength: dev.size,
    onload: null,
    onprogress: null,
    load() {
      this.onload?.({});
    },
    get(start, len, cb) {
      op(start, len, () => cb(read(start, len)));
    },
    set(start, data, cb) {
      const own = waiting.length || !dev.ready(start, data.length) ? data.slice() : data;
      op(start, own.length, () => {
        dev.write(start, own);
        cb?.();
      });
    },
    get_and_cache(start, len, cb) {
      op(start, len, () => cb(read(start, len)));
    },
    get_from_cache(start, len) {
      return read(start, len);
    },
    get_buffer(cb) {
      cb();
    },
    // Disk contents are saved by the runtime (overlays), not inside v86's own state.
    get_state() {
      return [];
    },
    set_state() {
    }
  };
}
async function runV86(env2) {
  const { profile, memory, restore } = env2.init;
  const cfg = profile.coreConfig;
  const { V86 } = await import(
    /* @vite-ignore */
    profile.core.module
  );
  const screen = new HeadlessScreen();
  const opts = {
    wasm_path: profile.core.wasm,
    memory_size: memory,
    vga_memory_size: cfg.vgaMemory ?? 8 << 20,
    acpi: cfg.acpi ?? false,
    autostart: true,
    disable_keyboard: true,
    disable_mouse: true,
    disable_speaker: true,
    ...cfg.options
  };
  if (cfg.offlineNic) {
    opts.net_device = { type: "ne2k", relay_url: "fetch" };
  }
  for (const slot of ["bios", "vga_bios"]) {
    const f = env2.files.get(slot);
    if (f) opts[slot] = { buffer: f.slice().buffer };
  }
  for (const [slot, id] of Object.entries(cfg.drives)) {
    const dev = env2.disks.get(id);
    if (!dev) throw new Error(`drive ${slot}: no disk ${id}`);
    opts[slot] = diskObject(dev);
  }
  if (restore) opts.initial_state = { buffer: restore.core };
  setInterval(() => {
    pumpFetches();
    completeReady();
  }, 4);
  const emu = new V86(opts);
  let adapter;
  Object.defineProperty(emu, "screen_adapter", {
    configurable: true,
    get: () => adapter,
    set: (v) => {
      adapter = Object.assign(v, screen.methods());
    }
  });
  screen.onResize = (w, h) => env2.setSource(screen.fb, w, h);
  screen.onDamage = (x, y, w, h) => env2.damage({ x, y, width: w, height: h });
  env2.setSource(screen.fb, screen.width, screen.height);
  emu.add_listener("vmware-absolute-mouse", (on) => env2.log(`vmware absolute mouse: ${on}`));
  await new Promise((res) => emu.add_listener("emulator-ready", () => res()));
  if (cfg.offlineNic) {
    self.fetch = () => Promise.reject(new TypeError("guest networking is disabled"));
  }
  screen.vga = emu.v86.cpu.devices.vga;
  const render = () => {
    if (screen.graphical) screen.vga.screen_fill_buffer();
    else {
      screen.vga.screen_fill_buffer();
      screen.renderText();
    }
  };
  if (restore) {
    render();
    env2.damage();
    env2.flush(true);
  }
  env2.started();
  const buttons = [false, false, false];
  let running = true, snapshotting = false, lastRender = 0, lastDiscrete = -Infinity, lastX = 0, lastY = 0;
  const sendDiscrete = (d) => {
    if (d.kind === "key") {
      const sc = PS2_SET1[KEY_CODES[d.code]];
      if (sc === void 0) return;
      const bytes = sc > 255 ? [224, sc & 255] : [sc];
      if (!d.down) bytes[bytes.length - 1] |= 128;
      emu.keyboard_send_scancodes(bytes);
    } else if (d.kind === "button") {
      buttons[d.index] = d.down;
      emu.bus.send("mouse-click", [buttons[0], buttons[2], buttons[1]]);
    } else {
      emu.bus.send("mouse-wheel", [d.dx, d.dy]);
    }
  };
  const tick = () => {
    if (env2.flag(FLAG.DEBUG)) {
      env2.clearFlag(FLAG.DEBUG);
      const cpu = emu.v86.cpu, ide = cpu.devices.ide?.primary?.master;
      env2.log("debug " + JSON.stringify({
        eip: (cpu.instruction_pointer[0] >>> 0).toString(16),
        hlt: cpu.in_hlt[0],
        if: !!(cpu.get_eflags?.() & 512),
        running,
        ide: ide && {
          status: ide.status_reg.toString(16),
          error: ide.error_reg,
          cmd: ide.current_command?.toString(16),
          dma: ide.channel?.dma_status,
          lba: [ide.lba_high_reg, ide.lba_mid_reg, ide.lba_low_reg]
        }
      }));
    }
    const { snapshot } = env2.poll(false);
    completeReady();
    if (env2.paused && running) {
      emu.stop();
      running = false;
    }
    if (!env2.paused && !running) {
      emu.run();
      running = true;
    }
    const now = performance.now();
    const { moved, discrete } = env2.takeInput(now - lastDiscrete < (cfg.eventIntervalMs ?? 8));
    if (moved) {
      let dx = env2.px - lastX;
      const dy = env2.py - lastY;
      if (!dx && !dy) dx = 1;
      lastX = env2.px;
      lastY = env2.py;
      emu.bus.send("mouse-delta", [dx, -dy]);
      emu.bus.send("mouse-absolute", [env2.px + 0.5, env2.py + 0.5, env2.width, env2.height]);
    }
    if (discrete) {
      lastDiscrete = now;
      sendDiscrete(discrete);
    }
    if (now - lastRender >= 16) {
      lastRender = now;
      render();
      env2.flush(false);
    }
    if (snapshot && !snapshotting && !waiting.length) {
      snapshotting = true;
      render();
      env2.damage();
      emu.save_state().then(
        (state) => {
          env2.sendSnapshot(state);
          snapshotting = false;
        },
        (e) => {
          env2.log(`snapshot failed: ${e}`);
          snapshotting = false;
        }
      );
    }
  };
  setInterval(tick, 4);
}

// src/worker/main.ts
var post = (m) => self.postMessage(m);
var env = null;
self.addEventListener("error", (e) => {
  const exit = e.error?.name === "ExitStatus" || /ExitStatus|exit\(\d+\)/.test(e.message ?? "");
  if (exit && env) env.halt();
  if (env?.halted) e.preventDefault();
});
self.addEventListener("unhandledrejection", (e) => {
  if (env?.halted) e.preventDefault();
});
self.onmessage = async ({ data }) => {
  if (data.type !== "init") return;
  let stage = "assets";
  try {
    startFetcher(data.ctrl, data.diskLatencyMs);
    const disks = /* @__PURE__ */ new Map();
    for (const d of data.disks) disks.set(d.id, await BlockDevice.open(d));
    const files = /* @__PURE__ */ new Map();
    await Promise.all(Object.entries(data.profile.machine.files ?? {}).map(async ([path, url]) => {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
      files.set(path, new Uint8Array(await res.arrayBuffer()));
    }));
    if (data.restore) clock.restore(data.restore.clock);
    else if (data.clockStart !== void 0) clock.start(data.clockStart);
    stage = "core";
    env = new Env(data, disks, files);
    if (data.profile.core.adapter === "macemu") await runMacemu(env);
    else await runV86(env);
  } catch (e) {
    if (env?.halted) return;
    post({ type: "fatal", kind: stage === "assets" ? "AssetError" : "CoreCrashedError", message: String(e?.stack ?? e) });
  }
};
self.addEventListener("unhandledrejection", (e) => post({ type: "log", text: `unhandled: ${e.reason}` }));
//# sourceMappingURL=webulator-worker.js.map
