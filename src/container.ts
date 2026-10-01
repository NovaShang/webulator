// The snapshot container (spec §6.2):
//   "WEBUSNAP" | u32 version | u32 header length | header JSON | sections
import type { Overlay, GuestClock } from "./protocol";
import { AssetError } from "./errors";

const MAGIC = "WEBUSNAP";
const VERSION = 1;

type Section = { name: string; codec: "raw" | "gzip"; length: number; rawLength: number };
export type SnapshotHeader = {
  profile: string; core: string; buildId: string; createdAt: string; memory: number;
  clock: GuestClock; screen: { width: number; height: number }; sections: Section[];
};
export type SnapshotParts = {
  header: Omit<SnapshotHeader, "sections">;
  core: ArrayBuffer;
  overlays: Record<string, Overlay>;
  frame: { width: number; height: number; rgba: ArrayBuffer };
};

async function gzip(buf: ArrayBuffer): Promise<ArrayBuffer> {
  return new Response(new Blob([buf]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer();
}
async function gunzip(buf: ArrayBuffer): Promise<ArrayBuffer> {
  return new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
}

export function encodeOverlay(o: Overlay): ArrayBuffer {
  const head = new Uint32Array(2 + o.chunks.length);
  head[0] = o.chunkSize; head[1] = o.chunks.length; head.set(o.chunks, 2);
  const out = new Uint8Array(head.byteLength + o.data.byteLength);
  out.set(new Uint8Array(head.buffer), 0); out.set(new Uint8Array(o.data), head.byteLength);
  return out.buffer;
}
export function decodeOverlay(buf: ArrayBuffer): Overlay {
  const v = new Uint32Array(buf, 0, 2), n = v[1];
  const chunks = Array.from(new Uint32Array(buf, 8, n));
  return { chunkSize: v[0], chunks, data: buf.slice(8 + n * 4) };
}

export async function packSnapshot(p: SnapshotParts): Promise<Blob> {
  const raw: [string, ArrayBuffer][] = [
    ["core", p.core],
    ...Object.entries(p.overlays).map(([id, o]) => [`disk:${id}`, encodeOverlay(o)] as [string, ArrayBuffer]),
    ["frame", p.frame.rgba],
  ];
  const packed = await Promise.all(raw.map(async ([name, buf]) => ({ name, rawLength: buf.byteLength, data: await gzip(buf) })));
  const header: SnapshotHeader = {
    ...p.header,
    sections: packed.map(s => ({ name: s.name, codec: "gzip", length: s.data.byteLength, rawLength: s.rawLength })),
  };
  const h = new TextEncoder().encode(JSON.stringify(header));
  const pre = new ArrayBuffer(16);
  new Uint8Array(pre).set(new TextEncoder().encode(MAGIC));
  new DataView(pre).setUint32(8, VERSION, true);
  new DataView(pre).setUint32(12, h.byteLength, true);
  return new Blob([pre, h, ...packed.map(s => s.data)], { type: "application/x-webulator-snapshot" });
}

export async function unpackSnapshot(buf: ArrayBuffer): Promise<SnapshotParts & { header: SnapshotHeader }> {
  const magic = new TextDecoder().decode(new Uint8Array(buf, 0, 8));
  if (magic !== MAGIC) throw new AssetError("not a Webulator snapshot");
  const dv = new DataView(buf);
  if (dv.getUint32(8, true) !== VERSION) throw new AssetError(`unsupported snapshot version ${dv.getUint32(8, true)}`);
  const hlen = dv.getUint32(12, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 16, hlen))) as SnapshotHeader;
  let p = 16 + hlen;
  const sections: Record<string, ArrayBuffer> = {};
  for (const s of header.sections) {
    const data = buf.slice(p, p + s.length); p += s.length;
    sections[s.name] = s.codec === "gzip" ? await gunzip(data) : data;
  }
  const overlays: Record<string, Overlay> = {};
  for (const [name, data] of Object.entries(sections)) if (name.startsWith("disk:")) overlays[name.slice(5)] = decodeOverlay(data);
  return { header, core: sections.core, overlays, frame: { ...header.screen, rgba: sections.frame } };
}
