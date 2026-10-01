// Shared control block between the main thread and a machine's worker (spec §4.1).
// The emulator loop may block the worker, so everything real-time goes through this SharedArrayBuffer.

export const CTRL = {
  FLAGS: 0,        // requests from the main thread, see FLAG
  WAKE: 1,         // bumped + notified on every input, wakes idle waits
  HEAD: 2,         // input ring write index (main thread)
  TAIL: 3,         // input ring read index (worker)
  PX: 4,           // latest absolute pointer position (coalesced, not in the ring)
  PY: 5,
  PSEQ: 6,         // bumped whenever PX/PY change
  PAUSED: 7,       // worker sets 1 while paused
  RING: 16,
} as const;

export const FLAG = {
  PAUSE: 1,
  SNAPSHOT: 2,
  OVERLAYS: 4,     // export disk overlays
  DEBUG: 8,        // diagnostics: the adapter logs internal state once
} as const;

export const RING_SIZE = 256;          // events
export const RING_STRIDE = 4;          // int32s per event: type, a, b, c
export const CTRL_INTS = CTRL.RING + RING_SIZE * RING_STRIDE;

export const EV = {
  KEY: 1,          // a = key index (KEY_CODES), b = down
  BUTTON: 2,       // a = button index, b = down
  MOVE: 3,         // a = dx, b = dy
  WHEEL: 4,        // a = dx, b = dy
} as const;

export function newControl(): { buf: SharedArrayBuffer; view: Int32Array } {
  const buf = new SharedArrayBuffer(CTRL_INTS * 4);
  return { buf, view: new Int32Array(buf) };
}

export type RingEvent = { type: number; a: number; b: number; c: number };

/** Worker side: read the next event without consuming it. */
export function peekEvent(v: Int32Array): RingEvent | null {
  const tail = Atomics.load(v, CTRL.TAIL);
  if (tail === Atomics.load(v, CTRL.HEAD)) return null;
  const i = CTRL.RING + (tail % RING_SIZE) * RING_STRIDE;
  return { type: v[i], a: v[i + 1], b: v[i + 2], c: v[i + 3] };
}

export function consumeEvent(v: Int32Array): void {
  Atomics.add(v, CTRL.TAIL, 1);
}

/** Main thread side: returns false when the ring is full. */
export function pushEvent(v: Int32Array, e: RingEvent): boolean {
  const head = Atomics.load(v, CTRL.HEAD);
  if (head - Atomics.load(v, CTRL.TAIL) >= RING_SIZE) return false;
  const i = CTRL.RING + (head % RING_SIZE) * RING_STRIDE;
  v[i] = e.type; v[i + 1] = e.a; v[i + 2] = e.b; v[i + 3] = e.c;
  Atomics.store(v, CTRL.HEAD, head + 1);
  wake(v);
  return true;
}

export function wake(v: Int32Array): void {
  Atomics.add(v, CTRL.WAKE, 1);
  Atomics.notify(v, CTRL.WAKE);
}
