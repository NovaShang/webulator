import { ProfileError, AssetError } from "./errors";
import type { Profile, DiskSpec } from "./types";

/** Validate a profile written in code. Throws ProfileError. */
export function defineProfile(p: Profile): Profile {
  const need = (ok: unknown, what: string) => { if (!ok) throw new ProfileError(`profile ${p?.id ?? "?"}: ${what}`); };
  need(p && typeof p.id === "string", "id is required");
  need(p.core && (p.core.adapter === "macemu" || p.core.adapter === "v86"), "core.adapter must be macemu or v86");
  need(typeof p.core.module === "string", "core.module is required");
  need(/^sha256:[0-9a-f]{64}$/.test(p.core.buildId ?? ""), "core.buildId must be sha256:<hex>");
  need(p.machine && p.machine.screen?.width > 0 && p.machine.screen?.height > 0, "machine.screen is required");
  need(p.machine.memory?.allowed?.includes(p.machine.memory.default), "machine.memory.default must be allowed");
  need(Array.isArray(p.disks) && p.disks.length > 0, "at least one disk");
  return p;
}

/** Fetch a profile JSON; relative URLs inside it resolve against the profile's own URL. */
export async function loadProfile(url: string | URL): Promise<Profile> {
  const base = new URL(url, globalThis.location?.href);
  const res = await fetch(base);
  if (!res.ok) throw new AssetError(`profile ${base}: HTTP ${res.status}`);
  const p = (await res.json()) as Profile;
  const abs = (u: string) => new URL(u, base).href;
  p.core.module = abs(p.core.module);
  if (p.core.wasm) p.core.wasm = abs(p.core.wasm);
  p.machine.files = Object.fromEntries(Object.entries(p.machine.files ?? {}).map(([k, v]) => [k, abs(v)]));
  p.disks = p.disks.map((d: DiskSpec) => {
    const s = d.source as Record<string, unknown>;
    if (typeof s.manifest === "string") return { ...d, source: { manifest: abs(s.manifest) } };
    if (typeof s.url === "string") return { ...d, source: { url: abs(s.url), size: s.size as number } };
    return d;
  });
  return defineProfile(p);
}
