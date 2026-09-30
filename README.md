# Webulator

One API for running old computers in the browser.

Webulator wraps WebAssembly emulators — v86 (x86), Mini vMac, Basilisk II (68k Mac) and SheepShaver (PowerPC Mac) to
start — behind a single browser API. Every core must pass the same conformance tests. The first user is
OS Museum, a project that shows operating systems from every era side by side.

**Status: design stage.** There is a v1 spec draft and a working prototype for the three Mac emulators. No package is
published yet.

## What it will do

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
- **Leave emulator source alone where possible.** Use build flags and a shared host runtime; when a patch is
  unavoidable, keep it small, pinned and upstreamable.

## What has been verified

Measured in headless Chromium on an Apple M2 (details in `docs/findings/`):

| Core | Guest | Snapshot, gzip | Restore to first frame | Speed cost | Source changes |
|---|---|---|---|---|---|
| Mini vMac | System 6.0.8 | 0.56 MB | ~0.1 s | none | none |
| Basilisk II | System 7.5.3 | 2.0 MB | ~0.16 s | none | none |
| SheepShaver | Mac OS 9.0.4 | 11.4 MB | ~0.5 s (worker: 60–80 ms) | ~15% | none |
| v86 | Windows 98 | native `save_state` | ~40 ms in-process | — | not yet wired in |

## Repository

| Path | What |
|---|---|
| `docs/spec.md` | The v1 API spec (draft, currently in Chinese) |
| `docs/backends.md` | What each emulator offers and what it lacks |
| `docs/findings/` | Measurement reports the spec is based on |
| `prototype/` | The working prototype: shared worker runtime, test page, build scripts |

## Licenses

This repository is MIT. The emulator cores are not included; when they are packaged they keep their own licenses
(v86: BSD-2-Clause; Mini vMac, Basilisk II, SheepShaver: GPL-2.0) and ship as separate packages. ROMs, BIOS files and
system images are never distributed here.
