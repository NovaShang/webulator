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
function newControl() {
  const buf = new SharedArrayBuffer(CTRL_INTS * 4);
  return { buf, view: new Int32Array(buf) };
}
function pushEvent(v, e) {
  const head = Atomics.load(v, CTRL.HEAD);
  if (head - Atomics.load(v, CTRL.TAIL) >= RING_SIZE) return false;
  const i = CTRL.RING + head % RING_SIZE * RING_STRIDE;
  v[i] = e.type;
  v[i + 1] = e.a;
  v[i + 2] = e.b;
  v[i + 3] = e.c;
  Atomics.store(v, CTRL.HEAD, head + 1);
  wake(v);
  return true;
}
function wake(v) {
  Atomics.add(v, CTRL.WAKE, 1);
  Atomics.notify(v, CTRL.WAKE);
}

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
function charToKey(ch) {
  const plain = "abcdefghijklmnopqrstuvwxyz";
  const i = plain.indexOf(ch);
  if (i >= 0) return ["Key" + ch.toUpperCase(), false];
  const j = plain.toUpperCase().indexOf(ch);
  if (j >= 0) return ["Key" + ch, true];
  const digits = "0123456789", shifted = ")!@#$%^&*(";
  if (digits.includes(ch)) return ["Digit" + ch, false];
  if (shifted.includes(ch)) return ["Digit" + shifted.indexOf(ch), true];
  const map = {
    " ": ["Space", false],
    "\n": ["Enter", false],
    "	": ["Tab", false],
    "-": ["Minus", false],
    "_": ["Minus", true],
    "=": ["Equal", false],
    "+": ["Equal", true],
    "[": ["BracketLeft", false],
    "{": ["BracketLeft", true],
    "]": ["BracketRight", false],
    "}": ["BracketRight", true],
    "\\": ["Backslash", false],
    "|": ["Backslash", true],
    ";": ["Semicolon", false],
    ":": ["Semicolon", true],
    "'": ["Quote", false],
    '"': ["Quote", true],
    "`": ["Backquote", false],
    "~": ["Backquote", true],
    ",": ["Comma", false],
    "<": ["Comma", true],
    ".": ["Period", false],
    ">": ["Period", true],
    "/": ["Slash", false],
    "?": ["Slash", true]
  };
  return map[ch] ?? null;
}

// src/errors.ts
var WebulatorError = class extends Error {
  constructor(message) {
    super(message);
    this.name = new.target.name;
  }
};
var ProfileError = class extends WebulatorError {
};
var AssetError = class extends WebulatorError {
};
var BuildMismatchError = class extends WebulatorError {
};
var SnapshotTimeoutError = class extends WebulatorError {
};
var UnsupportedInputError = class extends WebulatorError {
};
var CoreCrashedError = class extends WebulatorError {
};

// src/container.ts
var MAGIC = "WEBUSNAP";
var VERSION = 1;
async function gzip(buf) {
  return new Response(new Blob([buf]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer();
}
async function gunzip(buf) {
  return new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
}
function encodeOverlay(o) {
  const head = new Uint32Array(2 + o.chunks.length);
  head[0] = o.chunkSize;
  head[1] = o.chunks.length;
  head.set(o.chunks, 2);
  const out = new Uint8Array(head.byteLength + o.data.byteLength);
  out.set(new Uint8Array(head.buffer), 0);
  out.set(new Uint8Array(o.data), head.byteLength);
  return out.buffer;
}
function decodeOverlay(buf) {
  const v = new Uint32Array(buf, 0, 2), n = v[1];
  const chunks = Array.from(new Uint32Array(buf, 8, n));
  return { chunkSize: v[0], chunks, data: buf.slice(8 + n * 4) };
}
async function packSnapshot(p) {
  const raw = [
    ["core", p.core],
    ...Object.entries(p.overlays).map(([id, o]) => [`disk:${id}`, encodeOverlay(o)]),
    ["frame", p.frame.rgba]
  ];
  const packed = await Promise.all(raw.map(async ([name, buf]) => ({ name, rawLength: buf.byteLength, data: await gzip(buf) })));
  const header = {
    ...p.header,
    sections: packed.map((s) => ({ name: s.name, codec: "gzip", length: s.data.byteLength, rawLength: s.rawLength }))
  };
  const h = new TextEncoder().encode(JSON.stringify(header));
  const pre = new ArrayBuffer(16);
  new Uint8Array(pre).set(new TextEncoder().encode(MAGIC));
  new DataView(pre).setUint32(8, VERSION, true);
  new DataView(pre).setUint32(12, h.byteLength, true);
  return new Blob([pre, h, ...packed.map((s) => s.data)], { type: "application/x-webulator-snapshot" });
}
async function unpackSnapshot(buf) {
  const magic = new TextDecoder().decode(new Uint8Array(buf, 0, 8));
  if (magic !== MAGIC) throw new AssetError("not a Webulator snapshot");
  const dv = new DataView(buf);
  if (dv.getUint32(8, true) !== VERSION) throw new AssetError(`unsupported snapshot version ${dv.getUint32(8, true)}`);
  const hlen = dv.getUint32(12, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 16, hlen)));
  let p = 16 + hlen;
  const sections = {};
  for (const s of header.sections) {
    const data = buf.slice(p, p + s.length);
    p += s.length;
    sections[s.name] = s.codec === "gzip" ? await gunzip(data) : data;
  }
  const overlays = {};
  for (const [name, data] of Object.entries(sections)) if (name.startsWith("disk:")) overlays[name.slice(5)] = decodeOverlay(data);
  return { header, core: sections.core, overlays, frame: { ...header.screen, rgba: sections.frame } };
}

// src/attach.ts
var BUTTON_FROM_DOM = [0, 2, 1];
function attachCanvas(m, canvas, opts, frame) {
  const ctx = canvas.getContext("2d");
  let drawn = -1, raf = 0;
  const view = () => opts.viewport ?? { x: 0, y: 0, width: m.screen.width, height: m.screen.height };
  const draw = () => {
    raf = 0;
    const f = frame();
    if (!f || f.seq === drawn) return;
    drawn = f.seq;
    const v = view();
    if (canvas.width !== v.width || canvas.height !== v.height) {
      canvas.width = v.width;
      canvas.height = v.height;
    }
    ctx.putImageData(new ImageData(f.rgba, f.width, f.height), -v.x, -v.y, v.x, v.y, v.width, v.height);
  };
  const schedule = () => {
    if (!raf) raf = requestAnimationFrame(draw);
  };
  const off = [m.on("frame", schedule), m.on("resize", () => {
    drawn = -1;
    schedule();
  })];
  drawn = -1;
  schedule();
  const listeners = [];
  const listen = (t, type, fn) => {
    t.addEventListener(type, fn, { passive: false });
    listeners.push([t, type, fn]);
  };
  if (opts.input) {
    const prevCursor = canvas.style.cursor, prevTab = canvas.tabIndex;
    canvas.style.cursor = "none";
    canvas.tabIndex = canvas.tabIndex >= 0 ? canvas.tabIndex : 0;
    const toGuest = (e) => {
      const r = canvas.getBoundingClientRect(), v = view();
      return [v.x + (e.clientX - r.left) / r.width * v.width, v.y + (e.clientY - r.top) / r.height * v.height];
    };
    const buttons = m.info.hardware.pointerButtons;
    listen(canvas, "pointermove", (e) => {
      const [x, y] = toGuest(e);
      m.input.pointer.moveTo(x, y);
    });
    listen(canvas, "pointerdown", (e) => {
      canvas.focus();
      canvas.setPointerCapture(e.pointerId);
      const [x, y] = toGuest(e);
      m.input.pointer.moveTo(x, y);
      const b = BUTTON_FROM_DOM[e.button] ?? 0;
      if (b < buttons) m.input.pointer.button(b, true);
      e.preventDefault();
    });
    listen(canvas, "pointerup", (e) => {
      const b = BUTTON_FROM_DOM[e.button] ?? 0;
      if (b < buttons) m.input.pointer.button(b, false);
      e.preventDefault();
    });
    listen(canvas, "contextmenu", (e) => e.preventDefault());
    if (m.info.hardware.wheel) listen(canvas, "wheel", (e) => {
      m.input.pointer.wheel(Math.sign(e.deltaX), Math.sign(e.deltaY));
      e.preventDefault();
    });
    const key = (down) => (e) => {
      if (e.repeat && !down) return;
      try {
        m.input.key(e.code, down);
        e.preventDefault();
      } catch {
      }
    };
    listen(canvas, "keydown", key(true));
    listen(canvas, "keyup", key(false));
    listeners.push([canvas, "__restore", (() => {
      canvas.style.cursor = prevCursor;
      canvas.tabIndex = prevTab;
    })]);
  }
  return () => {
    off.forEach((f) => f());
    if (raf) cancelAnimationFrame(raf);
    for (const [t, type, fn] of listeners) {
      if (type === "__restore") fn();
      else t.removeEventListener(type, fn);
    }
  };
}

// src/machine.ts
var WORKER_URL = new URL("./webulator-worker.js?use-scheduling-api", import.meta.url);
var STARTUP_TIMEOUT = 12e4;
async function toBuffer(x) {
  if (x instanceof ArrayBuffer) return x;
  if (x instanceof Blob) return x.arrayBuffer();
  const res = await fetch(x.url);
  if (!res.ok) throw new AssetError(`${x.url}: HTTP ${res.status}`);
  return res.arrayBuffer();
}
var Machine = class _Machine {
  info;
  config;
  worker;
  ctrl;
  listeners = /* @__PURE__ */ new Map();
  _state = "running";
  frame = null;
  guestAt = { guest: 0, real: 0 };
  pendingSnapshot = null;
  pendingOverlays = null;
  pendingState = null;
  queue = [];
  flushTimer = 0;
  pseq = 0;
  detachFns = [];
  startGuest = 0;
  lastAccess = {};
  constructor(config, info) {
    this.config = config;
    this.info = info;
  }
  static async create(config) {
    const p = config.profile;
    if (!p) throw new ProfileError("profile is required");
    const memory = config.memory ?? p.machine.memory.default;
    if (!p.machine.memory.allowed.includes(memory)) throw new ProfileError(`memory ${memory} not allowed by ${p.id}`);
    const info = {
      profile: p.id,
      core: p.core.adapter,
      buildId: p.core.buildId,
      arch: p.machine.arch,
      machine: p.machine.name,
      hardware: p.machine.hardware,
      screen: { ...p.machine.screen },
      memory
    };
    const m = new _Machine(config, info);
    let restore;
    let overlays = {};
    if (config.snapshot) {
      const snap = await unpackSnapshot(await toBuffer(config.snapshot));
      if (snap.header.buildId !== p.core.buildId) throw new BuildMismatchError(`snapshot build ${snap.header.buildId}, core ${p.core.buildId}`);
      if (snap.header.memory !== memory) throw new ProfileError(`snapshot has ${snap.header.memory} B of memory, config ${memory}`);
      restore = { core: snap.core, clock: snap.header.clock };
      overlays = snap.overlays;
      m.frame = { ...snap.frame, rgba: new Uint8ClampedArray(snap.frame.rgba), seq: 0 };
    }
    await m.start(restore, overlays);
    if (config.display) m.screen.attach(config.display);
    return m;
  }
  async diskList(overlays) {
    const byId = new Map(this.config.profile.disks.map((d) => [d.id, d]));
    for (const d of this.config.disks ?? []) byId.set(d.id, d);
    return Promise.all([...byId.values()].map(async (d) => {
      let overlay = overlays[d.id];
      if (!overlay && d.overlay) overlay = decodeOverlay(await toBuffer(d.overlay));
      return { id: d.id, source: d.source, readOnly: !!d.readOnly, overlay };
    }));
  }
  async start(restore, overlays) {
    const { buf, view } = newControl();
    this.ctrl = view;
    this.pseq = 0;
    this.queue = [];
    const worker = new Worker(WORKER_URL, { type: "module" });
    this.worker = worker;
    const init = {
      type: "init",
      ctrl: buf,
      profile: this.config.profile,
      memory: this.info.memory,
      disks: await this.diskList(overlays),
      clockStart: this.config.clock?.start,
      restore,
      diskLatencyMs: this.config.debug?.diskLatencyMs
    };
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new CoreCrashedError("core did not start in time")), STARTUP_TIMEOUT);
      worker.onmessage = ({ data }) => {
        if (data.type === "started") {
          clearTimeout(timer);
          this.startGuest = restore ? restore.clock.date : data.guest;
          this.guestAt = { guest: data.guest, real: performance.now() };
          this._state = "running";
          resolve();
        } else if (data.type === "fatal") {
          clearTimeout(timer);
          reject(data.kind === "AssetError" ? new AssetError(data.message) : new CoreCrashedError(data.message));
        }
        this.onWorker(data);
      };
      worker.onerror = (e) => {
        clearTimeout(timer);
        reject(new CoreCrashedError(e.message));
      };
      const transfer = [];
      if (restore) transfer.push(restore.core);
      for (const d of init.disks) {
        if (d.overlay) transfer.push(d.overlay.data);
      }
      worker.postMessage(init, transfer);
    });
    worker.onerror = (e) => this.crash(e.message);
  }
  onWorker(m) {
    switch (m.type) {
      case "frame": {
        const resized = !this.frame || this.frame.width !== m.width || this.frame.height !== m.height;
        this.frame = { width: m.width, height: m.height, rgba: new Uint8ClampedArray(m.rgba), seq: m.seq };
        this.guestAt = { guest: m.guest, real: performance.now() };
        if (resized) {
          this.info.screen = { width: m.width, height: m.height };
          this.emit("resize", { width: m.width, height: m.height });
        }
        this.emit("frame", { seq: m.seq, dirty: m.dirty });
        break;
      }
      case "clock":
        this.guestAt = { guest: m.guest, real: performance.now() };
        break;
      case "state":
        this.guestAt = { guest: m.guest, real: performance.now() };
        this._state = m.state;
        this.emit("state", m.state);
        this.pendingState?.resolve(void 0);
        this.pendingState = null;
        break;
      case "snapshot":
        this.pendingSnapshot?.resolve(m);
        this.pendingSnapshot = null;
        break;
      case "overlays":
        this.lastAccess = m.access;
        this.pendingOverlays?.resolve(m.overlays);
        this.pendingOverlays = null;
        break;
      case "log":
        this.emit("log", m.text);
        break;
      case "crash":
        this.crash(m.message);
        break;
    }
  }
  crash(message) {
    if (this._state === "destroyed" || this._state === "crashed") return;
    this._state = "crashed";
    const err = new CoreCrashedError(message);
    this.emit("state", "crashed");
    this.emit("error", err);
    for (const p of [this.pendingSnapshot, this.pendingOverlays, this.pendingState]) p?.reject(err);
  }
  emit(k, e) {
    this.listeners.get(k)?.forEach((f) => f(e));
  }
  on(event, cb) {
    if (!this.listeners.has(event)) this.listeners.set(event, /* @__PURE__ */ new Set());
    this.listeners.get(event).add(cb);
    return () => this.listeners.get(event).delete(cb);
  }
  get state() {
    return this._state;
  }
  alive() {
    if (this._state === "destroyed") throw new WebulatorError("machine is destroyed");
    if (this._state === "crashed") throw new CoreCrashedError("machine has crashed");
  }
  // ---------- lifecycle ----------
  async pause() {
    this.alive();
    if (this._state === "paused") return;
    const done = new Promise((resolve, reject) => {
      this.pendingState = { resolve, reject };
    });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.PAUSE);
    wake(this.ctrl);
    await done;
  }
  async resume() {
    this.alive();
    if (this._state !== "paused") return;
    const done = new Promise((resolve, reject) => {
      this.pendingState = { resolve, reject };
    });
    Atomics.and(this.ctrl, CTRL.FLAGS, ~FLAG.PAUSE);
    wake(this.ctrl);
    await done;
  }
  async restart() {
    this.alive();
    const overlays = await this.requestOverlays();
    this.worker.terminate();
    this.frame = null;
    await this.start(void 0, overlays);
  }
  async destroy() {
    if (this._state === "destroyed") return;
    this.detachFns.forEach((f) => f());
    this.detachFns = [];
    clearInterval(this.flushTimer);
    this.worker.terminate();
    this._state = "destroyed";
    this.emit("state", "destroyed");
  }
  /** Diagnostics (not part of the spec): ask the core adapter to log its internal state ("log" events). */
  _debug() {
    this.alive();
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.DEBUG);
    wake(this.ctrl);
  }
  // ---------- screen ----------
  screen = /* @__PURE__ */ ((m) => ({
    get width() {
      return m.frame?.width ?? m.info.screen.width;
    },
    get height() {
      return m.frame?.height ?? m.info.screen.height;
    },
    /** Sequence number of the current frame; 0 means the frame came from a snapshot, not from the core yet. */
    get seq() {
      return m.frame?.seq ?? 0;
    },
    read: async (rect) => {
      const f = this.frame;
      if (!f) return new ImageData(this.info.screen.width, this.info.screen.height);
      const r = rect ?? { x: 0, y: 0, width: f.width, height: f.height };
      const out = new ImageData(r.width, r.height);
      for (let y = 0; y < r.height; y++) {
        const s = ((r.y + y) * f.width + r.x) * 4;
        out.data.set(f.rgba.subarray(s, s + r.width * 4), y * r.width * 4);
      }
      return out;
    },
    attach: (canvas, opts = {}) => {
      const detach = attachCanvas(this, canvas, opts, () => this.frame);
      this.detachFns.push(detach);
      return detach;
    },
    detach: () => {
      this.detachFns.forEach((f) => f());
      this.detachFns = [];
    }
  }))(this);
  // ---------- input ----------
  enqueue(e) {
    this.alive();
    if (this.queue.length === 0 && pushEvent(this.ctrl, e)) return;
    this.queue.push(e);
    if (!this.flushTimer) this.flushTimer = setInterval(() => {
      while (this.queue.length && pushEvent(this.ctrl, this.queue[0])) this.queue.shift();
      if (!this.queue.length) {
        clearInterval(this.flushTimer);
        this.flushTimer = 0;
      }
    }, 1);
  }
  /** Resolves when the worker has consumed every input sent so far. */
  drained() {
    return new Promise((res) => {
      const check = () => {
        if (!this.queue.length && Atomics.load(this.ctrl, CTRL.TAIL) === Atomics.load(this.ctrl, CTRL.HEAD)) res();
        else setTimeout(check, 2);
      };
      check();
    });
  }
  input = {
    key: (code, down) => {
      const i = KEY_INDEX.get(code);
      if (i === void 0) throw new UnsupportedInputError(`unknown key code ${code}`);
      this.enqueue({ type: EV.KEY, a: i, b: down ? 1 : 0, c: 0 });
    },
    type: async (text) => {
      for (const ch of text) {
        const k = charToKey(ch);
        if (!k) throw new UnsupportedInputError(`cannot type ${JSON.stringify(ch)} on a US layout`);
        const [code, shift] = k;
        if (shift) this.input.key("ShiftLeft", true);
        this.input.key(code, true);
        this.input.key(code, false);
        if (shift) this.input.key("ShiftLeft", false);
      }
      await this.drained();
    },
    pointer: {
      moveTo: (x, y) => {
        this.alive();
        const { width, height } = this.screen;
        Atomics.store(this.ctrl, CTRL.PX, Math.max(0, Math.min(width - 1, Math.round(x))));
        Atomics.store(this.ctrl, CTRL.PY, Math.max(0, Math.min(height - 1, Math.round(y))));
        Atomics.store(this.ctrl, CTRL.PSEQ, ++this.pseq);
        wake(this.ctrl);
      },
      move: (dx, dy) => this.enqueue({ type: EV.MOVE, a: Math.round(dx), b: Math.round(dy), c: 0 }),
      button: (index, down) => {
        if (index < 0 || index >= this.info.hardware.pointerButtons) throw new UnsupportedInputError(`${this.info.machine} has ${this.info.hardware.pointerButtons} pointer button(s)`);
        this.enqueue({ type: EV.BUTTON, a: index, b: down ? 1 : 0, c: 0 });
      },
      wheel: (dx, dy) => {
        if (!this.info.hardware.wheel) throw new UnsupportedInputError(`${this.info.machine} has no wheel`);
        this.enqueue({ type: EV.WHEEL, a: Math.round(dx), b: Math.round(dy), c: 0 });
      }
    }
  };
  // ---------- disks ----------
  requestOverlays() {
    this.alive();
    const p = new Promise((resolve, reject) => {
      this.pendingOverlays = { resolve, reject };
    });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.OVERLAYS);
    wake(this.ctrl);
    return p;
  }
  disks = {
    get: (id) => {
      const spec = [...this.config.disks ?? [], ...this.config.profile.disks].find((d) => d.id === id);
      if (!spec) throw new ProfileError(`no disk ${id}`);
      const m = this;
      return {
        id,
        readOnly: !!spec.readOnly,
        async exportOverlay() {
          const o = (await m.requestOverlays())[id];
          return new Blob([encodeOverlay(o)]);
        },
        /** Diagnostics (not part of the spec): base-image fetch statistics and first-touch order. */
        async access() {
          await m.requestOverlays();
          return m.lastAccess[id];
        }
      };
    }
  };
  // ---------- snapshots ----------
  async saveState(opts = {}) {
    this.alive();
    if (this.pendingSnapshot) throw new WebulatorError("a snapshot is already in progress");
    const got = new Promise((resolve, reject) => {
      this.pendingSnapshot = { resolve, reject };
    });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.SNAPSHOT);
    wake(this.ctrl);
    const timeout = opts.timeout ?? 2e3;
    let timer = 0;
    const s = await Promise.race([got, new Promise((_, reject) => {
      timer = setTimeout(() => {
        Atomics.and(this.ctrl, CTRL.FLAGS, ~FLAG.SNAPSHOT);
        this.pendingSnapshot = null;
        reject(new SnapshotTimeoutError(`no safe point within ${timeout} ms`));
      }, timeout);
    })]).finally(() => clearTimeout(timer));
    return packSnapshot({
      header: {
        profile: this.info.profile,
        core: this.info.core,
        buildId: this.info.buildId,
        createdAt: (/* @__PURE__ */ new Date()).toISOString(),
        memory: this.info.memory,
        clock: s.clock,
        screen: { width: s.frame.width, height: s.frame.height }
      },
      core: s.core,
      overlays: s.overlays,
      frame: s.frame
    });
  }
  async restoreState(snapshot) {
    this.alive();
    const snap = await unpackSnapshot(await toBuffer(snapshot));
    if (snap.header.buildId !== this.info.buildId) throw new BuildMismatchError(`snapshot build ${snap.header.buildId}, core ${this.info.buildId}`);
    this.worker.terminate();
    this.frame = { ...snap.frame, rgba: new Uint8ClampedArray(snap.frame.rgba), seq: 0 };
    await this.start({ core: snap.core, clock: snap.header.clock }, snap.overlays);
  }
  // ---------- clock ----------
  clock = {
    now: () => this.guestAt.guest + (this._state === "running" ? performance.now() - this.guestAt.real : 0),
    elapsed: () => this.clock.now() - this.startGuest
  };
};

// src/profile.ts
function defineProfile(p) {
  const need = (ok, what) => {
    if (!ok) throw new ProfileError(`profile ${p?.id ?? "?"}: ${what}`);
  };
  need(p && typeof p.id === "string", "id is required");
  need(p.core && (p.core.adapter === "macemu" || p.core.adapter === "v86"), "core.adapter must be macemu or v86");
  need(typeof p.core.module === "string", "core.module is required");
  need(/^sha256:[0-9a-f]{64}$/.test(p.core.buildId ?? ""), "core.buildId must be sha256:<hex>");
  need(p.machine && p.machine.screen?.width > 0 && p.machine.screen?.height > 0, "machine.screen is required");
  need(p.machine.memory?.allowed?.includes(p.machine.memory.default), "machine.memory.default must be allowed");
  need(Array.isArray(p.disks) && p.disks.length > 0, "at least one disk");
  return p;
}
async function loadProfile(url) {
  const base = new URL(url, globalThis.location?.href);
  const res = await fetch(base);
  if (!res.ok) throw new AssetError(`profile ${base}: HTTP ${res.status}`);
  const p = await res.json();
  const abs = (u) => new URL(u, base).href;
  p.core.module = abs(p.core.module);
  if (p.core.wasm) p.core.wasm = abs(p.core.wasm);
  p.machine.files = Object.fromEntries(Object.entries(p.machine.files ?? {}).map(([k, v]) => [k, abs(v)]));
  p.disks = p.disks.map((d) => {
    const s = d.source;
    if (typeof s.manifest === "string") return { ...d, source: { manifest: abs(s.manifest) } };
    if (typeof s.url === "string") return { ...d, source: { url: abs(s.url), size: s.size } };
    return d;
  });
  return defineProfile(p);
}
export {
  AssetError,
  BuildMismatchError,
  CoreCrashedError,
  Machine,
  ProfileError,
  SnapshotTimeoutError,
  UnsupportedInputError,
  WebulatorError,
  defineProfile,
  loadProfile
};
//# sourceMappingURL=webulator.js.map
