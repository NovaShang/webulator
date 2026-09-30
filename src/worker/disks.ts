// Synchronous block devices (spec §4.3). Cores read the disk synchronously from inside their main loop, so missing
// base chunks are fetched with synchronous XHR (allowed in workers). Writes go to a 4 KB overlay.
import type { Overlay, WorkerDisk } from "../protocol";

const OVERLAY_BLOCK = 4096;

type Base = {
  chunkSize: number;
  count: number;
  load(i: number): Uint8Array | null;        // null = all-zero chunk
};

function fetchSync(url: string, range?: [number, number]): Uint8Array {
  const x = new XMLHttpRequest();
  x.open("GET", url, false);
  x.responseType = "arraybuffer";
  if (range) x.setRequestHeader("Range", `bytes=${range[0]}-${range[1]}`);
  x.send();
  if (x.status !== 200 && x.status !== 206) throw new Error(`disk read ${url}: HTTP ${x.status}`);
  return new Uint8Array(x.response as ArrayBuffer);
}

type Manifest = { format: string; size: number; chunkSize: number; chunks: string[]; baseUrl?: string; prefetch?: number[] };

export class BlockDevice {
  readonly id: string;
  readonly size: number;
  readonly readOnly: boolean;
  private base: Base;
  private cache: (Uint8Array | null | undefined)[];
  private overlay = new Map<number, Uint8Array>();
  stats = { chunkFetches: 0, bytesFetched: 0, fetchMs: 0 };
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
      dev = new BlockDevice(d.id, buf.length, d.readOnly, { chunkSize: buf.length, count: 1, load: () => buf });
    } else if (typeof s.manifest === "string") {
      const res = await fetch(s.manifest);
      if (!res.ok) throw new Error(`disk manifest ${s.manifest}: HTTP ${res.status}`);
      const m = (await res.json()) as Manifest;
      const baseUrl = new URL(m.baseUrl ?? "chunks/", s.manifest).href;
      dev = new BlockDevice(d.id, m.size, d.readOnly, {
        chunkSize: m.chunkSize, count: m.chunks.length,
        load: i => (m.chunks[i] ? fetchSync(baseUrl + m.chunks[i]) : null),
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
        load: i => fetchSync(url, [i * cs, Math.min(size, (i + 1) * cs) - 1]),
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
      const t = performance.now();
      c = this.base.load(i);
      this.cache[i] = c;
      this.touched.push(i);
      if (c) { this.stats.chunkFetches++; this.stats.bytesFetched += c.length; this.stats.fetchMs += performance.now() - t; }
    }
    return c;
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
