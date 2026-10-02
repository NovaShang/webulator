// Block devices (spec §4.3). Missing base chunks are fetched in the background by a helper worker so the guest
// keeps running while it waits: a core asks ready(off, len) first and, when it says no, reports "busy" to the guest
// and asks again later. A core that reads without asking still works; it falls back to a synchronous XHR.
// Writes go to a 4 KB overlay.
import { CTRL } from "../control";
import type { Overlay, WorkerDisk } from "../protocol";

const OVERLAY_BLOCK = 4096;

type ChunkSource = { url: string; range?: [number, number] };
type Base = {
  chunkSize: number;
  count: number;
  source(i: number): ChunkSource | null;     // null = all-zero chunk
};

function fetchSync({ url, range }: ChunkSource): Uint8Array {
  const x = new XMLHttpRequest();
  x.open("GET", url, false);
  x.responseType = "arraybuffer";
  if (range) x.setRequestHeader("Range", `bytes=${range[0]}-${range[1]}`);
  x.send();
  if (x.status !== 200 && x.status !== 206) throw new Error(`disk read ${url}: HTTP ${x.status}`);
  return new Uint8Array(x.response as ArrayBuffer);
}

// ---------- background fetches ----------
// The core's thread may never return to the event loop (the Mac cores don't), so results cannot come back as
// messages. The helper worker writes each chunk into a shared slot, marks it done and bumps CTRL.WAKE, which
// also ends any idle wait in progress. The core thread collects finished slots in pump().
const SLOTS = 8, SLOT_SIZE = 1 << 20;
const FREE = 0, BUSY = 1, DONE = 2, FAILED = 3;
const FETCHER_SRC = `
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

type Job = { dev: BlockDevice; i: number; src: ChunkSource };

class Fetcher {
  private st = new Int32Array(new SharedArrayBuffer(SLOTS * 4));
  private lens = new Int32Array(new SharedArrayBuffer(SLOTS * 4));
  private data = new SharedArrayBuffer(SLOTS * SLOT_SIZE);
  private jobs: (Job | null)[] = new Array(SLOTS).fill(null);
  private queue: Job[] = [];
  private worker: Worker;
  constructor(ctrl: SharedArrayBuffer, latency: number) {
    this.worker = new Worker(URL.createObjectURL(new Blob([FETCHER_SRC], { type: "text/javascript" })));
    this.worker.postMessage({ init: { st: this.st.buffer, lens: this.lens.buffer, data: this.data, ctrl, latency } });
  }
  request(job: Job) { this.queue.push(job); this.dispatch(); }
  private dispatch() {
    for (let s = 0; s < SLOTS && this.queue.length; s++) {
      if (this.jobs[s]) continue;
      const job = this.queue.shift()!;
      this.jobs[s] = job; Atomics.store(this.st, s, BUSY);
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
      else if (state === FAILED) job.dev.deliver(job.i, undefined);
      else continue;
      this.jobs[s] = null; Atomics.store(this.st, s, FREE); freed = true;
    }
    if (freed) this.dispatch();
  }
}

let fetcher: Fetcher | null = null, testLatency = 0;
/** Start the background fetcher (once per worker). latencyMs adds a delay to every fetch, for tests. */
export function startFetcher(ctrl: SharedArrayBuffer, latencyMs = 0) { testLatency = latencyMs; fetcher ??= new Fetcher(ctrl, latencyMs); }
export function pumpFetches() { fetcher?.pump(); }

type Manifest = { format: string; size: number; chunkSize: number; chunks: string[]; baseUrl?: string; prefetch?: number[] };

export class BlockDevice {
  readonly id: string;
  readonly size: number;
  readonly readOnly: boolean;
  private base: Base;
  private cache: (Uint8Array | null | undefined)[];
  private inflight = new Set<number>();
  private failed = new Set<number>();       // background fetch failed: the next read fetches synchronously
  private overlay = new Map<number, Uint8Array>();
  stats = { chunkFetches: 0, bytesFetched: 0, fetchMs: 0, syncFetches: 0 };
  /** Base chunks in the order they were first needed; a prefetch list can be made from it. */
  touched: number[] = [];

  private constructor(id: string, size: number, readOnly: boolean, base: Base) {
    this.id = id; this.size = size; this.readOnly = readOnly; this.base = base;
    this.cache = new Array(base.count);
  }

  static async open(d: WorkerDisk): Promise<BlockDevice> {
    const s = d.source as Record<string, unknown>;
    let dev: BlockDevice;
    if (s.buffer instanceof ArrayBuffer) {
      const buf = new Uint8Array(s.buffer);
      dev = new BlockDevice(d.id, buf.length, d.readOnly, { chunkSize: buf.length, count: 1, source: () => null });
      dev.cache[0] = buf;
    } else if (typeof s.manifest === "string") {
      const res = await fetch(s.manifest);
      if (!res.ok) throw new Error(`disk manifest ${s.manifest}: HTTP ${res.status}`);
      const m = (await res.json()) as Manifest;
      const baseUrl = new URL(m.baseUrl ?? "chunks/", s.manifest).href;
      dev = new BlockDevice(d.id, m.size, d.readOnly, {
        chunkSize: m.chunkSize, count: m.chunks.length,
        source: i => (m.chunks[i] ? { url: baseUrl + m.chunks[i] } : null),
      });
      // Chunks the manifest marks as needed early are fetched up front, in parallel.
      await Promise.all((m.prefetch ?? []).filter(i => m.chunks[i]).map(async i => {
        const r = await fetch(baseUrl + m.chunks[i]);
        dev.cache[i] = new Uint8Array(await r.arrayBuffer());
      }));
    } else if (typeof s.url === "string" && typeof s.size === "number") {
      const url = s.url, size = s.size, cs = 256 * 1024;
      dev = new BlockDevice(d.id, size, d.readOnly, {
        chunkSize: cs, count: Math.ceil(size / cs),
        source: i => ({ url, range: [i * cs, Math.min(size, (i + 1) * cs) - 1] }),
      });
    } else {
      throw new Error(`disk ${d.id}: unknown source`);
    }
    if (d.overlay) dev.importOverlay(d.overlay);
    return dev;
  }

  private chunk(i: number): Uint8Array | null {
    let c = this.cache[i];
    if (c === undefined) {
      const src = this.base.source(i), t = performance.now();
      if (src && testLatency) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, testLatency);
      c = src ? fetchSync(src) : null;
      this.cache[i] = c; this.failed.delete(i);
      this.touched.push(i);
      if (c) { this.stats.chunkFetches++; this.stats.syncFetches++; this.stats.bytesFetched += c.length; this.stats.fetchMs += performance.now() - t; }
    }
    return c;
  }

  private fetchStarted = new Map<number, number>();
  /** A background fetch finished (data) or failed (undefined). */
  deliver(i: number, data: Uint8Array | undefined) {
    this.inflight.delete(i);
    if (this.cache[i] !== undefined) return;     // a synchronous read got there first
    if (!data) { this.failed.add(i); return; }
    this.cache[i] = data; this.touched.push(i);
    this.stats.chunkFetches++; this.stats.bytesFetched += data.length;
    this.stats.fetchMs += performance.now() - (this.fetchStarted.get(i) ?? performance.now());
  }

  /**
   * Whether [off, off + len) can be read or written now without a network wait. If not, the missing chunks are
   * requested in the background; ask again later. Overlay blocks that a range covers need no base data.
   */
  ready(off: number, len: number): boolean {
    pumpFetches();
    const cs = this.base.chunkSize, end = Math.min(this.size, off + len);
    let ok = true, prev = -1;
    for (let b = Math.floor(off / OVERLAY_BLOCK); b * OVERLAY_BLOCK < end; b++) {
      if (this.overlay.has(b)) continue;
      const first = Math.floor((b * OVERLAY_BLOCK) / cs), last = Math.floor((Math.min(end, (b + 1) * OVERLAY_BLOCK) - 1) / cs);
      for (let i = Math.max(first, prev + 1); i <= last; i++) { if (!this.has(i)) ok = false; prev = i; }
    }
    return ok;
  }

  /** Chunk i is cached, or will be read synchronously (no fetcher, or its fetch failed). Starts a fetch if not. */
  private has(i: number): boolean {
    if (this.cache[i] !== undefined || this.failed.has(i) || !fetcher) return true;
    const src = this.base.source(i);
    if (!src) { this.cache[i] = null; return true; }
    if (!this.inflight.has(i)) {
      this.inflight.add(i); this.fetchStarted.set(i, performance.now());
      fetcher.request({ dev: this, i, src });
    }
    return false;
  }

  /** Copy base-image bytes (no overlay) into dst. */
  private readBase(off: number, len: number, dst: Uint8Array, at: number) {
    const cs = this.base.chunkSize;
    while (len > 0) {
      const i = Math.floor(off / cs), o = off - i * cs, n = Math.min(len, cs - o);
      const c = this.chunk(i);
      if (c) dst.set(c.subarray(o, o + n), at); else dst.fill(0, at, at + n);
      off += n; at += n; len -= n;
    }
  }

  read(off: number, len: number, dst: Uint8Array, at = 0): void {
    if (this.overlay.size === 0) return this.readBase(off, len, dst, at);
    while (len > 0) {
      const b = Math.floor(off / OVERLAY_BLOCK), o = off - b * OVERLAY_BLOCK, n = Math.min(len, OVERLAY_BLOCK - o);
      const blk = this.overlay.get(b);
      if (blk) dst.set(blk.subarray(o, o + n), at); else this.readBase(off, n, dst, at);
      off += n; at += n; len -= n;
    }
  }

  write(off: number, src: Uint8Array): void {
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
      off += n; at += n; len -= n;
    }
  }

  exportOverlay(): Overlay {
    const chunks = [...this.overlay.keys()].sort((a, b) => a - b);
    const data = new Uint8Array(chunks.length * OVERLAY_BLOCK);
    chunks.forEach((c, i) => data.set(this.overlay.get(c)!, i * OVERLAY_BLOCK));
    return { chunkSize: OVERLAY_BLOCK, chunks, data: data.buffer };
  }

  importOverlay(o: Overlay): void {
    if (o.chunkSize !== OVERLAY_BLOCK) throw new Error(`overlay block size ${o.chunkSize} unsupported`);
    const data = new Uint8Array(o.data);
    o.chunks.forEach((c, i) => this.overlay.set(c, data.slice(i * OVERLAY_BLOCK, (i + 1) * OVERLAY_BLOCK)));
  }
}
