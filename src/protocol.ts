// Messages between the main thread and a machine worker. Main → worker only at start-up (the core may block the
// worker afterwards); worker → main at any time.
import type { Profile, Rect } from "./types";

export type Overlay = { chunkSize: number; chunks: number[]; data: ArrayBuffer };

export type DiskAccess = { chunkFetches: number; bytesFetched: number; fetchMs: number; touched: number[] };

export type WorkerDisk = {
  id: string;
  source: { manifest: string } | { url: string; size: number } | { buffer: ArrayBuffer };
  readOnly: boolean;
  overlay?: Overlay;
};

export type GuestClock = { date: number; perf: number };

export type InitMsg = {
  type: "init";
  ctrl: SharedArrayBuffer;
  profile: Profile;
  memory: number;
  disks: WorkerDisk[];
  clockStart?: number;
  restore?: { core: ArrayBuffer; clock: GuestClock };
};

export type FrameMsg = { type: "frame"; seq: number; width: number; height: number; rgba: ArrayBuffer; dirty: Rect[]; guest: number };
export type SnapshotMsg = {
  type: "snapshot";
  core: ArrayBuffer;
  overlays: Record<string, Overlay>;
  clock: GuestClock;
  frame: { width: number; height: number; rgba: ArrayBuffer };
};

export type WorkerMsg =
  | { type: "started"; width: number; height: number; guest: number }
  | FrameMsg
  | { type: "state"; state: "running" | "paused"; guest: number }
  | { type: "clock"; guest: number }
  | SnapshotMsg
  | { type: "overlays"; overlays: Record<string, Overlay>; access: Record<string, DiskAccess> }
  | { type: "log"; text: string }
  | { type: "crash"; message: string }
  | { type: "fatal"; kind: string; message: string };
