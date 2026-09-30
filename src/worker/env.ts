// What every core adapter gets from the runtime (spec §5 CoreEnv): control block, input, video, disks, snapshot
// plumbing. Adapters call poll() at least once per guest tick.
import { CTRL, FLAG, EV, peekEvent, consumeEvent } from "../control";
import type { InitMsg, WorkerMsg, Overlay } from "../protocol";
import type { Rect } from "../types";
import { clock, realPerf } from "./clock";
import type { BlockDevice } from "./disks";

const FRAME_INTERVAL = 16;   // ms between frame posts
const CLOCK_INTERVAL = 250;  // ms between clock heartbeats while no frames flow

export type Discrete = { kind: "key"; code: number; down: boolean } | { kind: "button"; index: number; down: boolean }
  | { kind: "wheel"; dx: number; dy: number };

export class Env {
  readonly ctrl: Int32Array;
  readonly init: InitMsg;
  readonly disks: Map<string, BlockDevice>;
  readonly files: Map<string, Uint8Array>;
  paused = false;

  // pointer state kept in guest pixels
  px = 0; py = 0;
  private pseq = 0;

  // video
  width = 0; height = 0;
  private src: Uint8Array | null = null;
  private fixAlpha = false;
  private dirty: Rect | null = null;
  private seq = 0;
  private lastPost = 0;
  private lastClock = 0;

  constructor(init: InitMsg, disks: Map<string, BlockDevice>, files: Map<string, Uint8Array>) {
    this.init = init; this.ctrl = new Int32Array(init.ctrl); this.disks = disks; this.files = files;
    this.width = init.profile.machine.screen.width; this.height = init.profile.machine.screen.height;
  }

  post(m: WorkerMsg, transfer: Transferable[] = []) { (self as unknown as Worker).postMessage(m, transfer); }
  log(text: string) { this.post({ type: "log", text }); }

  // ---------- lifecycle ----------
  started() { this.post({ type: "started", width: this.width, height: this.height, guest: Date.now() }); }
  crash(message: string) { if (!this.halted) this.post({ type: "crash", message }); }

  flag(bit: number) { return (Atomics.load(this.ctrl, CTRL.FLAGS) & bit) !== 0; }
  clearFlag(bit: number) { Atomics.and(this.ctrl, CTRL.FLAGS, ~bit); }

  /**
   * Called by the adapter at every input poll. Handles pause (blocking adapters wait here), overlay export,
   * frame and clock posting. Returns whether a snapshot was requested; the adapter takes it at a safe point.
   */
  poll(canBlock: boolean, allowSnapshot = true): { snapshot: boolean } {
    this.flush(false);
    if (this.flag(FLAG.OVERLAYS)) {
      this.clearFlag(FLAG.OVERLAYS);
      const overlays = this.exportOverlays();
      const access = Object.fromEntries([...this.disks].map(([id, d]) => [id, { ...d.stats, touched: d.touched }]));
      this.post({ type: "overlays", overlays, access }, Object.values(overlays).map(o => o.data));
    }
    if (this.flag(FLAG.PAUSE) && !this.paused) this.enterPause();
    if (this.paused && canBlock) {
      while (this.flag(FLAG.PAUSE)) {
        if (allowSnapshot && this.flag(FLAG.SNAPSHOT)) return { snapshot: true };   // snapshot while paused: clock stays frozen
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
      if (this.flag(FLAG.SNAPSHOT)) { this.clearFlag(FLAG.SNAPSHOT); this.log("snapshot requested after power-off; ignored"); }
      this.poll(false, false);
    }, 20);
  }

  enterPause() {
    this.paused = true; clock.pause(); Atomics.store(this.ctrl, CTRL.PAUSED, 1);
    this.flush(true);
    this.post({ type: "state", state: "paused", guest: Date.now() });
  }
  leavePause() {
    this.paused = false; clock.resume(); Atomics.store(this.ctrl, CTRL.PAUSED, 0);
    this.post({ type: "state", state: "running", guest: Date.now() });
  }

  /** Block until input arrives or `ms` pass (used by cores' idle waits). */
  idle(ms: number) {
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
  takeInput(holdDiscrete = false): { moved: boolean; discrete: Discrete | null } {
    let moved = false;
    const s = Atomics.load(this.ctrl, CTRL.PSEQ);
    if (s !== this.pseq) {
      this.pseq = s; this.px = Atomics.load(this.ctrl, CTRL.PX); this.py = Atomics.load(this.ctrl, CTRL.PY); moved = true;
    }
    for (;;) {
      const e = peekEvent(this.ctrl);
      if (!e) return { moved, discrete: null };
      if (e.type === EV.MOVE) {
        this.px = Math.max(0, Math.min(this.width - 1, this.px + e.a));
        this.py = Math.max(0, Math.min(this.height - 1, this.py + e.b));
        moved = true; consumeEvent(this.ctrl); continue;
      }
      if (holdDiscrete) return { moved, discrete: null };   // leave it queued, in order, for a later poll
      consumeEvent(this.ctrl);
      if (e.type === EV.KEY) return { moved, discrete: { kind: "key", code: e.a, down: e.b !== 0 } };
      if (e.type === EV.BUTTON) return { moved, discrete: { kind: "button", index: e.a, down: e.b !== 0 } };
      if (e.type === EV.WHEEL) return { moved, discrete: { kind: "wheel", dx: e.a, dy: e.b } };
    }
  }

  // ---------- video ----------
  /** The adapter's current RGBA framebuffer (a view; may live in wasm memory). */
  setSource(src: Uint8Array, width: number, height: number, fixAlpha = false) {
    const resized = width !== this.width || height !== this.height;
    this.src = src; this.width = width; this.height = height; this.fixAlpha = fixAlpha;
    if (resized) this.dirty = { x: 0, y: 0, width, height };
  }

  /** Mark part of the frame as changed (default: all of it). Posting is throttled. */
  damage(r?: Rect) {
    const full = { x: 0, y: 0, width: this.width, height: this.height };
    const d = r ?? full;
    if (!this.dirty) this.dirty = { ...d };
    else {
      const x0 = Math.min(this.dirty.x, d.x), y0 = Math.min(this.dirty.y, d.y);
      const x1 = Math.max(this.dirty.x + this.dirty.width, d.x + d.width), y1 = Math.max(this.dirty.y + this.dirty.height, d.y + d.height);
      this.dirty = { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
    }
  }

  copyFrame(): Uint8Array<ArrayBuffer> {
    const n = this.width * this.height * 4;
    const out = (this.src ? this.src.slice(0, n) : new Uint8Array(n)) as Uint8Array<ArrayBuffer>;
    if (this.fixAlpha) for (let i = 3; i < n; i += 4) out[i] = 255;
    return out;
  }

  flush(force: boolean) {
    const now = realPerf();
    if (this.dirty && this.src && (force || now - this.lastPost >= FRAME_INTERVAL)) {
      const rgba = this.copyFrame();
      const dirty = [this.dirty]; this.dirty = null; this.lastPost = now; this.lastClock = now;
      this.post({ type: "frame", seq: ++this.seq, width: this.width, height: this.height, rgba: rgba.buffer, dirty, guest: Date.now() }, [rgba.buffer]);
    } else if (now - this.lastClock >= CLOCK_INTERVAL) {
      this.lastClock = now;
      this.post({ type: "clock", guest: Date.now() });
    }
  }

  // ---------- disks & snapshots ----------
  exportOverlays(): Record<string, Overlay> {
    return Object.fromEntries([...this.disks].map(([id, d]) => [id, d.exportOverlay()]));
  }

  /** Called by the adapter once it holds its core state. Clears the request. */
  sendSnapshot(core: ArrayBuffer) {
    const overlays = this.exportOverlays();
    const rgba = this.copyFrame();
    this.clearFlag(FLAG.SNAPSHOT);
    this.post({ type: "snapshot", core, overlays, clock: clock.save(), frame: { width: this.width, height: this.height, rgba: rgba.buffer } },
      [core, rgba.buffer, ...Object.values(overlays).map(o => o.data)]);
  }
}
