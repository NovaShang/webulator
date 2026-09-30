// Adapter for Infinite Mac's Emscripten builds of Mini vMac, Basilisk II and SheepShaver (spec §5.1, "Asyncify" kind).
// These cores run a blocking main loop and call back into JS through a global `workerApi`. Snapshots unwind the wasm
// stack with Asyncify at the input poll, copy linear memory, and rewind into a fresh instance on restore.
import type { Env } from "../env";
import { KEY_CODES, ADB, MINIVMAC } from "../../keyboard";

type Config = {
  flavor: "minivmac" | "basilisk" | "sheepshaver";
  args?: string[];
  prefs: string;                      // template: ${memory} ${width} ${height} ${disks}
  keymap: "adb" | "minivmac";
  fixAlpha?: boolean;                 // Mini vMac leaves alpha at 0 for black
  guard?: { cpu: string; forbid?: string };   // snapshot only when the stack holds `cpu` once and no `forbid`
  // Minimum guest time between key/button events. Basilisk II and SheepShaver buffer only 16 ADB events per
  // 60 Hz interrupt, so events sent faster than this would be dropped.
  eventIntervalMs?: number;
};

type Asyncify = {
  state: number; currData: number;
  handleSleep(start: (wake: (v?: number) => void) => void): number;
  funcWrappers: Map<Function, Function>;
  callStackIdToFunc: Map<number, Function>; callstackFuncToId: Map<Function, number>; callStackId: number;
  doRewind(ptr: number): unknown;
};
type Snap = { asyncify: Asyncify | null; exports(): Record<string, unknown>; stackSave(): number; stackRestore(sp: number): void; startRewind(p: number): void };
type EmModule = Record<string, any> & { HEAPU8: Uint8Array; FS: any; snap?: Snap };

const ADDR = { BUTTON: 1, MOUSE_FLAG: 2, X: 3, Y: 4, KEY_FLAG: 5, KEYCODE: 6, KEYSTATE: 7, ZERO: 8 };
const REWINDING = 2;

export async function runMacemu(env: Env): Promise<void> {
  const { profile, memory, restore } = env.init;
  const cfg = profile.coreConfig as unknown as Config;
  const keymap = cfg.keymap === "minivmac" ? MINIVMAC : ADB;
  const W = profile.machine.screen.width, H = profile.machine.screen.height;
  let M: EmModule;
  let videoPtr = 0, videoW = W, videoH = H;
  const stage = new Int32Array(9);
  let buttonDown = false;

  (Error as unknown as { stackTraceLimit: number }).stackTraceLimit = 500;
  const cpuRe = cfg.guard ? new RegExp(cfg.guard.cpu, "g") : null;
  const forbidRe = cfg.guard?.forbid ? new RegExp(cfg.guard.forbid) : null;
  const atSafePoint = () => {
    if (!cpuRe) return true;
    const st = new Error().stack ?? "";
    return (st.match(cpuRe) ?? []).length === 1 && !(forbidRe && forbidRe.test(st));
  };

  const asyncify = () => M.snap?.asyncify ?? null;

  function captureCore(A: Asyncify): ArrayBuffer {
    const snap = M.snap!, ex = snap.exports();
    const id = new Int32Array(M.HEAPU8.buffer)[(A.currData + 8) >> 2];
    const rewindName = Object.keys(ex).find(k => ex[k] === A.funcWrappers.get(A.callStackIdToFunc.get(id)!));
    const meta = new TextEncoder().encode(JSON.stringify({ sp: snap.stackSave(), currData: A.currData, rewindId: id, rewindName, videoPtr, videoW, videoH }));
    const out = new Uint8Array(4 + meta.length + M.HEAPU8.length);
    new DataView(out.buffer).setUint32(0, meta.length, true);
    out.set(meta, 4); out.set(M.HEAPU8, 4 + meta.length);
    return out.buffer;
  }

  let lastDiscrete = -Infinity;
  function stageInput(): number {
    stage.fill(0); stage[ADDR.BUTTON] = -1;
    const now = performance.now();
    const { moved, discrete } = env.takeInput(now - lastDiscrete < (cfg.eventIntervalMs ?? 0));
    if (discrete) lastDiscrete = now;
    if (moved) { stage[ADDR.MOUSE_FLAG] = 1; stage[ADDR.X] = env.px; stage[ADDR.Y] = env.py; }
    if (discrete?.kind === "button" && discrete.index === 0) { stage[ADDR.BUTTON] = discrete.down ? 1 : 0; buttonDown = discrete.down; }
    if (discrete?.kind === "key") {
      const k = keymap[KEY_CODES[discrete.code]];
      if (k !== undefined) { stage[ADDR.KEY_FLAG] = 1; stage[ADDR.KEYCODE] = k; stage[ADDR.KEYSTATE] = discrete.down ? 1 : 0; }
    }
    return moved || discrete ? 1 : 0;
  }

  (self as any).workerApi = {
    InputBufferAddresses: {
      mouseButtonStateAddr: ADDR.BUTTON, mousePositionFlagAddr: ADDR.MOUSE_FLAG, mousePositionXAddr: ADDR.X,
      mousePositionYAddr: ADDR.Y, keyEventFlagAddr: ADDR.KEY_FLAG, keyCodeAddr: ADDR.KEYCODE, keyStateAddr: ADDR.KEYSTATE,
      speedFlagAddr: ADDR.ZERO, speedAddr: ADDR.ZERO, mouseDeltaXAddr: ADDR.ZERO, mouseDeltaYAddr: ADDR.ZERO,
      useMouseDeltasFlagAddr: ADDR.ZERO, useMouseDeltasAddr: ADDR.ZERO, ethernetInterruptFlagAddr: ADDR.ZERO,
    },
    acquireInputLock() {
      const A = asyncify();
      if (A && A.state === REWINDING) return A.handleSleep(() => {});   // end of a rewind (restore or wake-up)
      const { snapshot } = env.poll(true);
      if (snapshot && A && atSafePoint()) {
        return A.handleSleep(wake => {
          setTimeout(() => {              // the wasm stack is unwound: the whole machine is in linear memory
            try { env.sendSnapshot(captureCore(A)); } catch (e) { env.log(`snapshot failed: ${e}`); }
            wake(0);
          }, 0);
        });
      }
      return stageInput();
    },
    getInputValue: (addr: number) => stage[addr],
    releaseInputLock() {},
    sleep(sec: number) { env.flush(false); env.idle(sec * 1000); },
    // Idle path: honour pause and overlay requests here too (not snapshots: this is not a safe point).
    idleWait() { env.poll(true, false); env.idle(8); return env.hasInput() ? 1 : 0; },
    didOpenVideo(w: number, h: number) { videoW = w; videoH = h; },
    blit(ptr: number, _size: number, rect?: { top: number; left: number; bottom: number; right: number }) {
      if (ptr) {
        if (ptr !== videoPtr || !env.width) { videoPtr = ptr; env.setSource(M.HEAPU8.subarray(ptr, ptr + videoW * videoH * 4), videoW, videoH, !!cfg.fixAlpha); }
        env.damage(rect ? { x: rect.left, y: rect.top, width: rect.right - rect.left, height: rect.bottom - rect.top } : undefined);
      }
      env.flush(false);
    },
    didOpenAudio() {}, enqueueAudio() { return 0; }, audioBufferSize() { return 1 << 20; },
    getClipboardText: () => "", setClipboardText() {},
    etherSeed: () => 1, etherInit() {}, etherWrite() {}, etherRead: () => 0,
    disks: {
      open(name: string) { const id = name.replace(/^\*?\/disk\//, ""); return [...env.disks.keys()].indexOf(id); },
      close() {},
      size: (i: number) => [...env.disks.values()][i].size,
      read(i: number, ptr: number, off: number, len: number) { [...env.disks.values()][i].read(off, len, M.HEAPU8, ptr); return len; },
      write(i: number, ptr: number, off: number, len: number) { [...env.disks.values()][i].write(off, M.HEAPU8.subarray(ptr, ptr + len)); return len; },
      consumeDiskName: () => null,
      isMediaPresent: () => 1, isFixedDisk: () => 1, eject() {},
    },
  };
  void buttonDown;

  const prefs = cfg.prefs
    .replaceAll("${memory}", String(memory)).replaceAll("${width}", String(W)).replaceAll("${height}", String(H))
    .replaceAll("${disks}", [...env.disks.keys()].map(id => `disk /disk/${id}`).join("\n"));

  M = {
    arguments: cfg.args ?? [],
    noInitialRun: !!restore,
    print: (t: string) => env.log(t), printErr: (t: string) => env.log(t),
    onAbort: (what: unknown) => env.crash(String(what)),
    // Guest power-off makes the core call exit(). Mark the machine halted first; the ExitStatus Emscripten then
    // throws (and any trap after it) is swallowed by the worker's error handler, and the worker keeps serving.
    onExit: () => env.halt(),
    quit: (_status: number, toThrow: unknown) => { env.halt(); throw toThrow; },
    preRun: [(mod: EmModule) => {
      for (const [path, bytes] of env.files) mod.FS.writeFile(path, bytes);
      mod.FS.writeFile("/prefs", prefs);
    }],
  } as unknown as EmModule;

  const factory = (await import(/* @vite-ignore */ profile.core.module)).default as (m: EmModule) => Promise<EmModule>;

  if (!restore) {
    env.started();
    await factory(M);          // main() runs inside and blocks this worker for the rest of its life
    return;
  }

  await factory(M);            // noInitialRun: the runtime is up, main() has not been called
  const A = asyncify();
  if (!A) throw new Error("this core build has no Asyncify; it cannot restore snapshots");
  const core = new Uint8Array(restore.core);
  const mlen = new DataView(restore.core).getUint32(0, true);
  const meta = JSON.parse(new TextDecoder().decode(core.subarray(4, 4 + mlen)));
  const mem = core.subarray(4 + mlen);
  if (mem.length !== M.HEAPU8.length) throw new Error(`snapshot memory ${mem.length} B, core has ${M.HEAPU8.length} B`);
  M.HEAPU8.set(mem);
  M.snap!.stackRestore(meta.sp);
  const orig = [...A.funcWrappers].find(([, w]) => w === M.snap!.exports()[meta.rewindName])?.[0];
  if (!orig) throw new Error("snapshot rewind entry not found in this build");
  A.callStackIdToFunc.set(meta.rewindId, orig); A.callstackFuncToId.set(orig, meta.rewindId);
  A.callStackId = Math.max(A.callStackId, meta.rewindId + 1);
  A.currData = meta.currData;
  videoPtr = meta.videoPtr; videoW = meta.videoW; videoH = meta.videoH;
  if (videoPtr) { env.setSource(M.HEAPU8.subarray(videoPtr, videoPtr + videoW * videoH * 4), videoW, videoH, !!cfg.fixAlpha); env.damage(); }
  env.flush(true);             // first frame, straight from restored memory
  env.started();
  A.state = REWINDING;
  M.snap!.startRewind(A.currData);
  A.doRewind(A.currData);      // re-enters main() and runs down the saved stack; does not return
}
