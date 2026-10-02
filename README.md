# Webulator

One API for running old computers in the browser.

**Live demo: https://novashang.github.io/webulator/** — Windows 98 (x86), System 6 and System 7.5 (68k), Mac OS 9 (PowerPC).

Webulator wraps WebAssembly emulators — v86 (x86), Mini vMac, Basilisk II (68k Mac) and SheepShaver (PowerPC Mac) to
start — behind a single browser API. Every core must pass the same conformance tests. The first user is
OS Museum, a project that shows operating systems from every era side by side.

**Status: v1 implemented, not yet published.** The v1 API runs four profiles (Mini vMac · System 6.0.8,
Basilisk II · System 7.5.3, SheepShaver · Mac OS 9.0.4, v86 · Windows 98) and all of them pass the full conformance
suite (`docs/conformance.md`).

## Usage

```ts
const m = await Machine.create({
  profile: await loadProfile(".../sheepshaver-g3bw-macos904.json"),
  disks: [{ id: "hd0", source: { manifest: ".../macos904.manifest.json" } }],
  snapshot: { url: ".../finder.webusnap" },   // start from a saved state instead of booting
  display: canvas,
});
m.input.pointer.moveTo(20, 8);                 // exact, in guest pixels
m.input.pointer.button(0, true);
const state = await m.saveState();             // restore it later, in a fresh page
```

## Principles

- **Strictly equal capabilities.** A capability enters the contract only when every core implements it and passes the
  same tests. There are no optional capabilities. Hardware differences (CPU, mouse buttons, screen sizes) are exposed
  as properties, not as missing features.
- **Snapshots first.** Save a running machine and restore it into a new instance, first frame identical to the pixel.
- **A virtual clock.** The guest never sees time pass while it is paused or stored.
- **Disk fetches never stall the guest.** Disk images load lazily over the network; while a chunk is on its way the
  guest keeps running and taking interrupts (its cursor keeps moving), as a real machine does while its disk is busy.
- **Leave emulator source alone where possible.** Use build flags and a shared host runtime; when a patch is
  unavoidable, keep it small, pinned and upstreamable.

## What has been verified

Headless Chromium 153 on an Apple M2, all twelve conformance tests per profile (details in `docs/conformance.md`):

| Profile | Cold boot | Snapshot (gzip) | Restore → first frame | First frame vs saved | Emulator source changes |
|---|---|---|---|---|---|
| Mini vMac · System 6.0.8 | 2.7 s | 0.57 MB | 46–85 ms | identical | disk driver: report "busy" instead of blocking (small patch) |
| Basilisk II · System 7.5.3 | 4.0 s | 2.0 MB | 77–140 ms | identical | disk driver: report "busy" instead of blocking (small patch) |
| SheepShaver · Mac OS 9.0.4 | 27.9 s | 11.1 MB | 158–217 ms | identical | disk driver: report "busy" instead of blocking (small patch) |
| v86 · Windows 98 | 19.1 s | 12.4 MB | 137–170 ms | identical | none |

## Repository

| Path | What |
|---|---|
| `src/` | The library: `Machine` API (main thread), worker runtime, core adapters (`macemu`, `v86`) |
| `profiles/` | The four verified profiles |
| `conformance/` | The conformance suite (T1–T12), per-profile hooks, Playwright runner |
| `tools/` | Build, dev server, disk chunking, build ids, overlay merge |
| `docs/spec.md` | The v1 API spec (currently in Chinese) |
| `docs/conformance.md` | Results, test method, profile preparation, known issues |
| `docs/backends.md` | What each emulator offers and what it lacks |
| `docs/findings/` | Measurement reports the spec is based on |
| `prototype/` | The earlier spike code and the emulator build scripts |

## Build and test

```sh
npm install
npm run build            # dist/webulator.js + dist/webulator-worker.js
npm run conformance      # needs cores, ROMs and disk images in assets/ (see profiles/*.json)
./tools/deploy-pages.sh  # build site/ and publish it to the gh-pages branch
```

## Licenses

This repository is MIT. The emulator cores are not included; when they are packaged they keep their own licenses
(v86: BSD-2-Clause; Mini vMac, Basilisk II, SheepShaver: GPL-2.0) and ship as separate packages. ROMs, BIOS files and
system images are never distributed here.
