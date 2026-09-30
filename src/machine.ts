import { CTRL, FLAG, EV, newControl, pushEvent, wake, type RingEvent } from "./control";
import { KEY_INDEX, charToKey } from "./keyboard";
import { packSnapshot, unpackSnapshot, encodeOverlay, decodeOverlay } from "./container";
import type { InitMsg, WorkerMsg, WorkerDisk, Overlay, SnapshotMsg, DiskAccess } from "./protocol";
import type { MachineConfig, MachineInfo, MachineState, MachineEvents, Rect, AttachOptions, DiskSpec } from "./types";
import { ProfileError, AssetError, BuildMismatchError, SnapshotTimeoutError, UnsupportedInputError, CoreCrashedError, WebulatorError } from "./errors";
import { attachCanvas } from "./attach";

type Listener<K extends keyof MachineEvents> = (e: MachineEvents[K]) => void;
type Pending = { resolve: (v: any) => void; reject: (e: Error) => void };

const WORKER_URL = new URL("./webulator-worker.js", import.meta.url);
const STARTUP_TIMEOUT = 120_000;

async function toBuffer(x: Blob | ArrayBuffer | { url: string }): Promise<ArrayBuffer> {
  if (x instanceof ArrayBuffer) return x;
  if (x instanceof Blob) return x.arrayBuffer();
  const res = await fetch(x.url);
  if (!res.ok) throw new AssetError(`${x.url}: HTTP ${res.status}`);
  return res.arrayBuffer();
}

export class Machine {
  readonly info: MachineInfo;
  private config: MachineConfig;
  private worker!: Worker;
  private ctrl!: Int32Array;
  private listeners = new Map<string, Set<Function>>();
  private _state: MachineState = "running";
  private frame: { width: number; height: number; rgba: Uint8ClampedArray; seq: number } | null = null;
  private guestAt = { guest: 0, real: 0 };
  private pendingSnapshot: Pending | null = null;
  private pendingOverlays: Pending | null = null;
  private pendingState: Pending | null = null;
  private queue: RingEvent[] = [];
  private flushTimer = 0;
  private pseq = 0;
  private detachFns: (() => void)[] = [];
  private startGuest = 0;
  private lastAccess: Record<string, DiskAccess> = {};

  private constructor(config: MachineConfig, info: MachineInfo) { this.config = config; this.info = info; }

  static async create(config: MachineConfig): Promise<Machine> {
    const p = config.profile;
    if (!p) throw new ProfileError("profile is required");
    const memory = config.memory ?? p.machine.memory.default;
    if (!p.machine.memory.allowed.includes(memory)) throw new ProfileError(`memory ${memory} not allowed by ${p.id}`);
    const info: MachineInfo = {
      profile: p.id, core: p.core.adapter, buildId: p.core.buildId, arch: p.machine.arch, machine: p.machine.name,
      hardware: p.machine.hardware, screen: { ...p.machine.screen }, memory,
    };
    const m = new Machine(config, info);
    let restore: InitMsg["restore"];
    let overlays: Record<string, Overlay> = {};
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

  private async diskList(overlays: Record<string, Overlay>): Promise<WorkerDisk[]> {
    const byId = new Map<string, DiskSpec>(this.config.profile.disks.map(d => [d.id, d]));
    for (const d of this.config.disks ?? []) byId.set(d.id, d);
    return Promise.all([...byId.values()].map(async d => {
      let overlay = overlays[d.id];
      if (!overlay && d.overlay) overlay = decodeOverlay(await toBuffer(d.overlay));
      return { id: d.id, source: d.source, readOnly: !!d.readOnly, overlay };
    }));
  }

  private async start(restore: InitMsg["restore"], overlays: Record<string, Overlay>): Promise<void> {
    const { buf, view } = newControl();
    this.ctrl = view; this.pseq = 0; this.queue = [];
    const worker = new Worker(WORKER_URL, { type: "module" });
    this.worker = worker;
    const init: InitMsg = {
      type: "init", ctrl: buf, profile: this.config.profile, memory: this.info.memory,
      disks: await this.diskList(overlays), clockStart: this.config.clock?.start, restore,
    };
    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => reject(new CoreCrashedError("core did not start in time")), STARTUP_TIMEOUT);
      worker.onmessage = ({ data }: MessageEvent<WorkerMsg>) => {
        if (data.type === "started") {
          clearTimeout(timer); this.startGuest = restore ? restore.clock.date : data.guest;
          this.guestAt = { guest: data.guest, real: performance.now() };
          this._state = "running"; resolve();
        } else if (data.type === "fatal") {
          clearTimeout(timer);
          reject(data.kind === "AssetError" ? new AssetError(data.message) : new CoreCrashedError(data.message));
        }
        this.onWorker(data);
      };
      worker.onerror = e => { clearTimeout(timer); reject(new CoreCrashedError(e.message)); };
      const transfer: Transferable[] = [];
      if (restore) transfer.push(restore.core);
      for (const d of init.disks) { if (d.overlay) transfer.push(d.overlay.data); }
      worker.postMessage(init, transfer);
    });
    worker.onerror = e => this.crash(e.message);
  }

  private onWorker(m: WorkerMsg) {
    switch (m.type) {
      case "frame": {
        const resized = !this.frame || this.frame.width !== m.width || this.frame.height !== m.height;
        this.frame = { width: m.width, height: m.height, rgba: new Uint8ClampedArray(m.rgba), seq: m.seq };
        this.guestAt = { guest: m.guest, real: performance.now() };
        if (resized) { this.info.screen = { width: m.width, height: m.height }; this.emit("resize", { width: m.width, height: m.height }); }
        this.emit("frame", { seq: m.seq, dirty: m.dirty });
        break;
      }
      case "clock": this.guestAt = { guest: m.guest, real: performance.now() }; break;
      case "state":
        this.guestAt = { guest: m.guest, real: performance.now() };
        this._state = m.state; this.emit("state", m.state);
        this.pendingState?.resolve(undefined); this.pendingState = null;
        break;
      case "snapshot": this.pendingSnapshot?.resolve(m); this.pendingSnapshot = null; break;
      case "overlays": this.lastAccess = m.access; this.pendingOverlays?.resolve(m.overlays); this.pendingOverlays = null; break;
      case "log": this.emit("log", m.text); break;
      case "crash": this.crash(m.message); break;
    }
  }

  private crash(message: string) {
    if (this._state === "destroyed" || this._state === "crashed") return;
    this._state = "crashed";
    const err = new CoreCrashedError(message);
    this.emit("state", "crashed"); this.emit("error", err);
    for (const p of [this.pendingSnapshot, this.pendingOverlays, this.pendingState]) p?.reject(err);
  }

  private emit<K extends keyof MachineEvents>(k: K, e: MachineEvents[K]) { this.listeners.get(k)?.forEach(f => (f as Listener<K>)(e)); }

  on<K extends keyof MachineEvents>(event: K, cb: Listener<K>): () => void {
    if (!this.listeners.has(event)) this.listeners.set(event, new Set());
    this.listeners.get(event)!.add(cb);
    return () => this.listeners.get(event)!.delete(cb);
  }

  get state(): MachineState { return this._state; }

  private alive() {
    if (this._state === "destroyed") throw new WebulatorError("machine is destroyed");
    if (this._state === "crashed") throw new CoreCrashedError("machine has crashed");
  }

  // ---------- lifecycle ----------
  async pause(): Promise<void> {
    this.alive(); if (this._state === "paused") return;
    const done = new Promise((resolve, reject) => { this.pendingState = { resolve, reject }; });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.PAUSE); wake(this.ctrl);
    await done;
  }
  async resume(): Promise<void> {
    this.alive(); if (this._state !== "paused") return;
    const done = new Promise((resolve, reject) => { this.pendingState = { resolve, reject }; });
    Atomics.and(this.ctrl, CTRL.FLAGS, ~FLAG.PAUSE); wake(this.ctrl);
    await done;
  }
  async restart(): Promise<void> {
    this.alive();
    const overlays = await this.requestOverlays();
    this.worker.terminate();
    this.frame = null;
    await this.start(undefined, overlays);
  }
  async destroy(): Promise<void> {
    if (this._state === "destroyed") return;
    this.detachFns.forEach(f => f()); this.detachFns = [];
    clearInterval(this.flushTimer);
    this.worker.terminate();
    this._state = "destroyed"; this.emit("state", "destroyed");
  }

  // ---------- screen ----------
  readonly screen = ((m: Machine) => ({
    get width() { return m.frame?.width ?? m.info.screen.width; },
    get height() { return m.frame?.height ?? m.info.screen.height; },
    /** Sequence number of the current frame; 0 means the frame came from a snapshot, not from the core yet. */
    get seq() { return m.frame?.seq ?? 0; },
    read: async (rect?: Rect): Promise<ImageData> => {
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
    attach: (canvas: HTMLCanvasElement, opts: AttachOptions = {}) => {
      const detach = attachCanvas(this, canvas, opts, () => this.frame);
      this.detachFns.push(detach);
      return detach;
    },
    detach: () => { this.detachFns.forEach(f => f()); this.detachFns = []; },
  }))(this);

  // ---------- input ----------
  private enqueue(e: RingEvent) {
    this.alive();
    if (this.queue.length === 0 && pushEvent(this.ctrl, e)) return;
    this.queue.push(e);
    if (!this.flushTimer) this.flushTimer = setInterval(() => {
      while (this.queue.length && pushEvent(this.ctrl, this.queue[0])) this.queue.shift();
      if (!this.queue.length) { clearInterval(this.flushTimer); this.flushTimer = 0; }
    }, 1) as unknown as number;
  }

  /** Resolves when the worker has consumed every input sent so far. */
  private drained(): Promise<void> {
    return new Promise(res => {
      const check = () => {
        if (!this.queue.length && Atomics.load(this.ctrl, CTRL.TAIL) === Atomics.load(this.ctrl, CTRL.HEAD)) res();
        else setTimeout(check, 2);
      };
      check();
    });
  }

  readonly input = {
    key: (code: string, down: boolean) => {
      const i = KEY_INDEX.get(code);
      if (i === undefined) throw new UnsupportedInputError(`unknown key code ${code}`);
      this.enqueue({ type: EV.KEY, a: i, b: down ? 1 : 0, c: 0 });
    },
    type: async (text: string) => {
      for (const ch of text) {
        const k = charToKey(ch);
        if (!k) throw new UnsupportedInputError(`cannot type ${JSON.stringify(ch)} on a US layout`);
        const [code, shift] = k;
        if (shift) this.input.key("ShiftLeft", true);
        this.input.key(code, true); this.input.key(code, false);
        if (shift) this.input.key("ShiftLeft", false);
      }
      await this.drained();
    },
    pointer: {
      moveTo: (x: number, y: number) => {
        this.alive();
        const { width, height } = this.screen;
        Atomics.store(this.ctrl, CTRL.PX, Math.max(0, Math.min(width - 1, Math.round(x))));
        Atomics.store(this.ctrl, CTRL.PY, Math.max(0, Math.min(height - 1, Math.round(y))));
        Atomics.store(this.ctrl, CTRL.PSEQ, ++this.pseq);
        wake(this.ctrl);
      },
      move: (dx: number, dy: number) => this.enqueue({ type: EV.MOVE, a: Math.round(dx), b: Math.round(dy), c: 0 }),
      button: (index: number, down: boolean) => {
        if (index < 0 || index >= this.info.hardware.pointerButtons) throw new UnsupportedInputError(`${this.info.machine} has ${this.info.hardware.pointerButtons} pointer button(s)`);
        this.enqueue({ type: EV.BUTTON, a: index, b: down ? 1 : 0, c: 0 });
      },
      wheel: (dx: number, dy: number) => {
        if (!this.info.hardware.wheel) throw new UnsupportedInputError(`${this.info.machine} has no wheel`);
        this.enqueue({ type: EV.WHEEL, a: Math.round(dx), b: Math.round(dy), c: 0 });
      },
    },
  };

  // ---------- disks ----------
  private requestOverlays(): Promise<Record<string, Overlay>> {
    this.alive();
    const p = new Promise<Record<string, Overlay>>((resolve, reject) => { this.pendingOverlays = { resolve, reject }; });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.OVERLAYS); wake(this.ctrl);
    return p;
  }

  readonly disks = {
    get: (id: string) => {
      const spec = [...(this.config.disks ?? []), ...this.config.profile.disks].find(d => d.id === id);
      if (!spec) throw new ProfileError(`no disk ${id}`);
      const m = this;
      return {
        id, readOnly: !!spec.readOnly,
        async exportOverlay(): Promise<Blob> { const o = (await m.requestOverlays())[id]; return new Blob([encodeOverlay(o)]); },
        /** Diagnostics (not part of the spec): base-image fetch statistics and first-touch order. */
        async access(): Promise<DiskAccess> { await m.requestOverlays(); return m.lastAccess[id]; },
      };
    },
  };

  // ---------- snapshots ----------
  async saveState(opts: { timeout?: number } = {}): Promise<Blob> {
    this.alive();
    if (this.pendingSnapshot) throw new WebulatorError("a snapshot is already in progress");
    const got = new Promise<SnapshotMsg>((resolve, reject) => { this.pendingSnapshot = { resolve, reject }; });
    Atomics.or(this.ctrl, CTRL.FLAGS, FLAG.SNAPSHOT); wake(this.ctrl);
    const timeout = opts.timeout ?? 2000;
    let timer = 0;
    const s = await Promise.race([got, new Promise<never>((_, reject) => {
      timer = setTimeout(() => {
        Atomics.and(this.ctrl, CTRL.FLAGS, ~FLAG.SNAPSHOT);
        this.pendingSnapshot = null;
        reject(new SnapshotTimeoutError(`no safe point within ${timeout} ms`));
      }, timeout) as unknown as number;
    })]).finally(() => clearTimeout(timer));
    return packSnapshot({
      header: { profile: this.info.profile, core: this.info.core, buildId: this.info.buildId, createdAt: new Date().toISOString(),
        memory: this.info.memory, clock: s.clock, screen: { width: s.frame.width, height: s.frame.height } },
      core: s.core, overlays: s.overlays, frame: s.frame,
    });
  }

  async restoreState(snapshot: Blob | ArrayBuffer): Promise<void> {
    this.alive();
    const snap = await unpackSnapshot(await toBuffer(snapshot));
    if (snap.header.buildId !== this.info.buildId) throw new BuildMismatchError(`snapshot build ${snap.header.buildId}, core ${this.info.buildId}`);
    this.worker.terminate();
    this.frame = { ...snap.frame, rgba: new Uint8ClampedArray(snap.frame.rgba), seq: 0 };
    await this.start({ core: snap.core, clock: snap.header.clock }, snap.overlays);
  }

  // ---------- clock ----------
  readonly clock = {
    now: () => this.guestAt.guest + (this._state === "running" ? performance.now() - this.guestAt.real : 0),
    elapsed: () => this.clock.now() - this.startGuest,
  };
}
