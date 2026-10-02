export type Rect = { x: number; y: number; width: number; height: number };

export type Arch = "x86" | "m68k" | "ppc";

export type Hardware = {
  pointerButtons: number;   // 1 for classic Macs, 3 for PCs
  wheel: boolean;
  keyboard: string;         // "adb-extended" | "mac-plus" | "pc-at" …
};

/** A fixed combination of core + machine + system image (spec §6.3). URLs are resolved against the profile's URL. */
export type Profile = {
  id: string;
  name: string;
  core: {
    adapter: "macemu" | "v86";
    module: string;          // the core's ES module (Emscripten output, or libv86.mjs)
    wasm?: string;           // v86 only: v86.wasm
    buildId: string;         // "sha256:…" of the core's wasm; snapshots are bound to it
  };
  machine: {
    name: string;
    arch: Arch;
    screen: { width: number; height: number };
    memory: { default: number; allowed: number[] };
    hardware: Hardware;
    files: Record<string, string>;   // guest-visible file path (or v86 slot) → URL: ROM, BIOS …
  };
  disks: DiskSpec[];
  coreConfig: Record<string, unknown>;   // passed to the adapter as is
};

export type DiskSource =
  | { manifest: string }
  | { url: string; size: number }
  | { buffer: ArrayBuffer };

export type DiskSpec = {
  id: string;
  source: DiskSource;
  readOnly?: boolean;
  overlay?: Blob | ArrayBuffer;
};

export type MachineConfig = {
  profile: Profile;
  memory?: number;
  disks?: DiskSpec[];                     // defaults to profile.disks; entries override by id
  snapshot?: { url: string } | Blob | ArrayBuffer;
  clock?: { start?: number };             // epoch ms
  display?: HTMLCanvasElement;
  /** Test hooks; not part of the contract. diskLatencyMs delays every background disk fetch. */
  debug?: { diskLatencyMs?: number };
};

export type MachineState = "running" | "paused" | "destroyed" | "crashed";

export type MachineInfo = {
  profile: string;
  core: string;
  buildId: string;
  arch: Arch;
  machine: string;
  hardware: Hardware;
  screen: { width: number; height: number };
  memory: number;
};

export type FrameEvent = { seq: number; dirty: Rect[] };

export type MachineEvents = {
  state: MachineState;
  resize: { width: number; height: number };
  frame: FrameEvent;
  error: Error;
  log: string;
};

export type AttachOptions = {
  input?: boolean;           // forward pointer / keyboard / wheel events from the canvas to the guest
  viewport?: Rect;           // show and map only this part of the guest screen
};
