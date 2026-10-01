// Worker entry. The clock import must stay first: it replaces Date.now / performance.now before any core loads.
import "./clock";
import { clock } from "./clock";
import type { InitMsg, WorkerMsg } from "../protocol";
import { BlockDevice } from "./disks";
import { Env } from "./env";
import { runMacemu } from "./adapters/macemu";
import { runV86 } from "./adapters/v86";

const post = (m: WorkerMsg) => (self as unknown as Worker).postMessage(m);

let env: Env | null = null;
// After the guest powers off, the core's exit path throws (ExitStatus, or a trap). That is not a crash.
self.addEventListener("error", e => {
  const exit = (e.error as { name?: string } | null)?.name === "ExitStatus" || /ExitStatus|exit\(\d+\)/.test(e.message ?? "");
  if (exit && env) env.halt();
  if (env?.halted) e.preventDefault();
});
self.addEventListener("unhandledrejection", e => { if (env?.halted) e.preventDefault(); });

self.onmessage = async ({ data }: MessageEvent<InitMsg>) => {
  if (data.type !== "init") return;
  let stage = "assets";
  try {
    const disks = new Map<string, BlockDevice>();
    for (const d of data.disks) disks.set(d.id, await BlockDevice.open(d));
    const files = new Map<string, Uint8Array>();
    await Promise.all(Object.entries(data.profile.machine.files ?? {}).map(async ([path, url]) => {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
      files.set(path, new Uint8Array(await res.arrayBuffer()));
    }));
    if (data.restore) clock.restore(data.restore.clock);
    else if (data.clockStart !== undefined) clock.start(data.clockStart);

    stage = "core";
    env = new Env(data, disks, files);
    if (data.profile.core.adapter === "macemu") await runMacemu(env);
    else await runV86(env);
  } catch (e) {
    if (env?.halted) return;
    post({ type: "fatal", kind: stage === "assets" ? "AssetError" : "CoreCrashedError", message: String((e as Error)?.stack ?? e) });
  }
};

self.addEventListener("unhandledrejection", e => post({ type: "log", text: `unhandled: ${(e as PromiseRejectionEvent).reason}` }));
