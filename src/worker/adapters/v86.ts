// Adapter for v86 (spec §5.1, "native snapshot" kind). v86 schedules itself on the worker's event loop, so this
// adapter polls the control block on a timer instead of from inside a blocking core loop.
import type { Env, Discrete } from "../env";
import type { BlockDevice } from "../disks";
import { KEY_CODES, PS2_SET1 } from "../../keyboard";
import { HeadlessScreen } from "./v86screen";

type Config = {
  vgaMemory?: number;
  acpi?: boolean;
  drives: Record<string, string>;     // v86 slot (hda, hdb, cdrom, fda) → disk id
  options?: Record<string, unknown>;  // extra V86 constructor options (hardware the guest image expects)
  // An NE2000 card whose DHCP requests are answered locally, with no traffic leaving the worker. For images that
  // wait for a DHCP lease at boot. Networking itself is not part of the v1 contract.
  offlineNic?: boolean;
  eventIntervalMs?: number;           // minimum guest time between key/button events (default 8)
};

function diskObject(dev: BlockDevice) {
  const read = (start: number, len: number) => { const d = new Uint8Array(len); dev.read(start, len, d); return d; };
  return {
    byteLength: dev.size,
    onload: null as null | ((e: unknown) => void), onprogress: null,
    load() { this.onload?.({}); },
    get(start: number, len: number, cb: (d: Uint8Array) => void) { const d = read(start, len); queueMicrotask(() => cb(d)); },
    set(start: number, data: Uint8Array, cb?: () => void) { dev.write(start, data); cb?.(); },
    get_and_cache(start: number, len: number, cb: (d: Uint8Array) => void) { cb(read(start, len)); },
    get_from_cache(start: number, len: number) { return read(start, len); },
    get_buffer(cb: (b?: ArrayBuffer) => void) { cb(); },
    // Disk contents are saved by the runtime (overlays), not inside v86's own state.
    get_state() { return []; },
    set_state() {},
  };
}

export async function runV86(env: Env): Promise<void> {
  const { profile, memory, restore } = env.init;
  const cfg = profile.coreConfig as unknown as Config;
  const { V86 } = await import(/* @vite-ignore */ profile.core.module);
  const screen = new HeadlessScreen();

  const opts: Record<string, unknown> = {
    wasm_path: profile.core.wasm,
    memory_size: memory,
    vga_memory_size: cfg.vgaMemory ?? 8 << 20,
    acpi: cfg.acpi ?? false,
    autostart: true,
    disable_keyboard: true, disable_mouse: true, disable_speaker: true,
    ...cfg.options,
  };
  if (cfg.offlineNic) {
    opts.net_device = { type: "ne2k", relay_url: "fetch" };
    // v86's fetch relay answers ARP/DHCP itself and uses fetch() for everything else; fetch is closed below.
  }
  for (const slot of ["bios", "vga_bios"]) {
    const f = env.files.get(slot);
    if (f) opts[slot] = { buffer: f.slice().buffer };
  }
  for (const [slot, id] of Object.entries(cfg.drives)) {
    const dev = env.disks.get(id);
    if (!dev) throw new Error(`drive ${slot}: no disk ${id}`);
    opts[slot] = diskObject(dev);
  }
  if (restore) opts.initial_state = { buffer: restore.core };

  const emu = new V86(opts);
  // v86 creates its (dummy) screen adapter later, during async init; swap our methods in as it is assigned.
  let adapter: unknown;
  Object.defineProperty(emu, "screen_adapter", {
    configurable: true,
    get: () => adapter,
    set: (v: object) => { adapter = Object.assign(v, screen.methods()); },
  });
  screen.onResize = (w, h) => env.setSource(screen.fb, w, h, true);
  screen.onDamage = (x, y, w, h) => env.damage({ x, y, width: w, height: h });
  env.setSource(screen.fb, screen.width, screen.height, true);
  emu.add_listener("vmware-absolute-mouse", (on: boolean) => env.log(`vmware absolute mouse: ${on}`));

  await new Promise<void>(res => emu.add_listener("emulator-ready", () => res()));
  if (cfg.offlineNic) {
    // All assets are loaded by now; the runtime reads disks with XHR. Guest traffic must not reach the network.
    (self as any).fetch = () => Promise.reject(new TypeError("guest networking is disabled"));
  }
  screen.vga = emu.v86.cpu.devices.vga;

  const render = () => {
    if (screen.graphical) screen.vga!.screen_fill_buffer(); else { screen.vga!.screen_fill_buffer(); screen.renderText(); }
  };
  if (restore) { render(); env.damage(); env.flush(true); }
  env.started();

  const buttons = [false, false, false];   // API index: 0 primary, 1 secondary, 2 middle
  let running = true, snapshotting = false, lastRender = 0, lastDiscrete = -Infinity, lastX = 0, lastY = 0;

  const sendDiscrete = (d: Discrete) => {
    if (d.kind === "key") {
      const sc = PS2_SET1[KEY_CODES[d.code]];
      if (sc === undefined) return;                     // a key this machine does not have
      const bytes = sc > 0xff ? [0xe0, sc & 0xff] : [sc];
      if (!d.down) bytes[bytes.length - 1] |= 0x80;
      emu.keyboard_send_scancodes(bytes);
    } else if (d.kind === "button") {
      buttons[d.index] = d.down;
      emu.bus.send("mouse-click", [buttons[0], buttons[2], buttons[1]]);
    } else {
      emu.bus.send("mouse-wheel", [d.dx, d.dy]);
    }
  };

  const tick = () => {
    const { snapshot } = env.poll(false);
    if (env.paused && running) { emu.stop(); running = false; }
    if (!env.paused && !running) { emu.run(); running = true; }

    // At most one key/button event per interval: Windows' keyboard buffer overflows if a burst arrives at once.
    const now = performance.now();
    const { moved, discrete } = env.takeInput(now - lastDiscrete < (cfg.eventIntervalMs ?? 8));
    if (moved) {
      // The absolute position goes through the VMware backdoor, but the guest driver only reads it when a PS/2
      // mouse interrupt arrives, so send the matching relative move first (as v86's own browser mouse does).
      let dx = env.px - lastX; const dy = env.py - lastY;
      if (!dx && !dy) dx = 1;
      lastX = env.px; lastY = env.py;
      emu.bus.send("mouse-delta", [dx, -dy]);
      emu.bus.send("mouse-absolute", [env.px + 0.5, env.py + 0.5, env.width, env.height]);
    }
    if (discrete) { lastDiscrete = now; sendDiscrete(discrete); }

    if (now - lastRender >= 16) { lastRender = now; render(); env.flush(false); }

    if (snapshot && !snapshotting) {
      // Render and save in the same task, so the saved frame matches the saved state exactly.
      snapshotting = true;
      render(); env.damage();
      emu.save_state().then((state: ArrayBuffer) => { env.sendSnapshot(state); snapshotting = false; },
        (e: unknown) => { env.log(`snapshot failed: ${e}`); snapshotting = false; });
    }
  };
  setInterval(tick, 4);
}
