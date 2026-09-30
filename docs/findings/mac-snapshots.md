> Imported from the OS Museum project (2026-09). Paths such as `spikes/…`, `vendor/…` and `images/…` refer to that project; the reusable code now lives in `prototype/` here.

# S2 · Snapshot and restore for classic Mac emulators, without touching their source

Part 1: Mini vMac (below). Part 2: Basilisk II (further down).

## Part 1 · Mini vMac

Date: 2026-09-27 · Apple M2 · headless Chromium 153 (Playwright) · Emscripten 4.0.22
Emulator: Mini vMac from Infinite Mac's fork (`vendor/minivmac` @ d57bcea), Mac Plus, all-out speed · Guest: System 6.0.8

## Result

**It works.** A Mac saved in one worker was restored into a brand-new worker on a fresh page load. It kept running:
the Apple menu opened, and Calculator opened, which means its code was read from the restored disk.
The first frame after restore matched the screen at save time pixel for pixel. **No emulator source was changed.**

| | |
|---|---|
| Source changes | none |
| Build changes | Asyncify on 5 outer-loop functions, plus a 9-line `--pre-js` |
| wasm size | 264,457 → 275,480 bytes (+4%) |
| Boot to Finder, median of 10 | 0.487 s without Asyncify, 0.463 s with it (no measurable slowdown) |
| Save (request → snapshot in hand) | 27–63 ms (unwind 2–4 ms, memory copy 17–43 ms) |
| Snapshot file | 48.0 MB raw; **gzip 562 KB, zstd -3 441 KB, zstd -19 327 KB** |
| Restore in a fresh page (worker start → first frame), median of 5 | 37 ms (raw file), 39 ms (gzip) |
| Page start → first frame, file served locally | 122 ms raw, 93 ms gzip (fetch 8 ms + decompress 37 ms) |
| First frame vs screen at save time | 0 pixels differ (10 of 10 runs) |
| Alive after restore | 10 of 10 runs: Apple menu opens, Calculator opens |

Menu timings are ~100 ms because the host sends frames at most every 100 ms. That delay comes from the spike, not from the emulator.

## How it works

1. Mini vMac's main loop never returns to JS, and the wasm call stack is not in linear memory. So a memory dump cannot be
   resumed as-is. Asyncify fixes this for one call path: `main → ProgramMain → MainEventLoop → WaitForNextTick → ReadJSInput`.
   The build flags are `-sASYNCIFY -sASYNCIFY_IMPORTS=[env.emscripten_asm_const_int] -sASYNCIFY_ONLY=[those functions]`.
   The CPU core is not instrumented, which is why speed does not change.
2. To save: the page sets a flag in a SharedArrayBuffer. The next time the emulator polls input (`workerApi.acquireInputLock`,
   once per tick), our JS calls `Asyncify.handleSleep`. The stack unwinds into linear memory and the worker returns to its
   event loop. We then copy the whole wasm memory, the stack pointer, the Asyncify data pointer and the export to rewind into.
   Disk writes are tracked in 4 KB chunks, and only the written ones are saved (1 chunk here).
3. To restore: a new worker instantiates the module with `noInitialRun` and writes the memory back. It sets the stack pointer,
   rebuilds Asyncify's rewind-id table, and calls `asyncify_start_rewind` + `doRewind`. The emulator re-enters `main` and runs
   down the saved stack to the input poll, then carries on. The base disk image is loaded separately, because it is shared
   and cacheable, and the written chunks are applied on top.

## What this means for the library

- The "patch the source" option for Mac snapshots is not needed for Mini vMac. A build-flag change plus our own
  `workerApi` is enough.
- Snapshots are tied to one exact build: memory layout and rewind ids must match. That is fine for the museum, because the
  exhibit factory regenerates snapshots whenever a core is rebuilt. The library should store a build hash in every snapshot
  and refuse a mismatch.
- The 48 MB comes from `INITIAL_MEMORY` and is mostly zeros. The compressed size (0.3–0.6 MB) is what goes over the network.
  Basilisk II / SheepShaver use 288 MB and larger guest RAM, so their snapshots will be much larger; measure next.

## Open items

- **Optimized builds crash the guest.** At -O1, -O2 and -O3, with or without Asyncify, System 6 bombs at boot
  ("error type 7" / "coprocessor not installed"). Adding `-fno-strict-aliasing -fwrapv` does not help. Infinite Mac also
  ships -O0 builds. Likely undefined behaviour in the CPU core; worth fixing later for speed, but not needed for correctness.
- Guest clock: after a restore Mini vMac sees a large time jump, treats it as "emulation interrupted" and skips it (see
  `OSGLUESC.c` `UpdateTrueEmulatedTime`). The Mac's date jumps to the current host date. A virtual clock would make this exact.
- Audio state, Ethernet, and host-side state beyond disks and video were not needed here and are not captured.
- The first attempt at restoring while the old, busy worker was still being terminated took ~1.6 s. From a fresh page it is
  ~40 ms. Worth re-checking in S5 (two machines side by side).
- Next: SheepShaver (nested `execute`, 68k/PPC mode switching).

## Files

- `build/build.sh`: builds `out/base*.mjs` and `out/async*.mjs` in `emscripten/emsdk:4.0.22-arm64`. Env: `OPT`, `SUF`, `DBG`.
- `build/post.js`: the `--pre-js` that exposes Asyncify, the exports and stack helpers.
- `web/mac-worker.mjs`: our `workerApi` host, snapshot and restore.
- `web/index.html`: `?mode=boot|make|restore`. Run with `node serve.mjs . 8767`, then `../s0-engine-bench/web/run-page.mjs`.
- `results/*.json`, `web/uploads/*.png` (screens: desktop, restored, restored-calculator).

## Part 2 · Basilisk II (2026-09-27)

Emulator: Basilisk II from Infinite Mac's macemu fork, built at -O3 exactly as Infinite Mac ships it (incl. gmp/mpfr FPU).
Machine: Quadra 650 (68040), 32 MB guest RAM, 640×480 · Guest: System 7.5.3. Same host (`web/emu-worker.mjs`) as Mini vMac.

**It works too, still with no source change.** Restored in a fresh page: first frame identical to the screen at save time,
Apple menu and Calculator work, 5 of 5 runs.

| | base | Asyncify, full | **Asyncify, 3 functions** |
|---|---|---|---|
| Asyncify scope | none | every function that can reach JS | `main`, `m68k_execute()`, `ReadJSInput(long long)` |
| wasm size | 1,713,959 | 2,552,713 (+49%) | 1,747,136 (+2%) |
| Boot, desktop, median | 0.78 s (n=12) | 0.90 s (n=6) | 0.83 s (n=6) |
| Boot, settled, median | 1.48 s | 1.66 s (+12%) | **1.47 s (same)** |

Snapshot (narrowed build):

| `INITIAL_MEMORY` | raw file | gzip | zstd -19 | save | restore, fresh page → first frame (median of 5) |
|---|---|---|---|---|---|
| 288 MB (Infinite Mac default) | 288 MB | 2.2 MB | 1.66 MB | 118–151 ms | 230–340 ms (worker 130–240 ms) |
| **80 MB** (enough for 32 MB guest RAM) | 80 MB | **2.0 MB** | **1.65 MB** | 36 ms | **157 ms from gzip** (fetch 10 + gunzip 51 + restore 79), 255 ms from raw |

### Two things this part found

1. **Nested CPU loops.** Basilisk II re-enters `m68k_execute` when native code calls 68k code (`Execute68k`). With Asyncify
   on only the outer path, those nested frames can't unwind. The host checks the JS stack trace (`m68k_execute` must appear
   once) and otherwise defers the snapshot to the next input poll. In practice it was never deferred at an idle desktop.
2. **The guest clock must not see the gap.** Without a fix, a snapshot restored a minute or more later is sluggish:
   Calculator took 5–8 s to open instead of ~15 ms. Cause: every ~166 ms Basilisk II recalibrates how many instructions to
   run between clock checks from `instructions / elapsed time` (`main_unix.cpp` `cpu_do_check_ticks`). The gap between
   save and restore counts as elapsed time, the quantum collapses to ~0, and it checks the clock on almost every
   instruction. **Fix, no patch:** the worker wraps `Date.now` / `performance.now` (all Emscripten clock imports go through
   them), records the clock in the snapshot and offsets it on restore. The guest sees no time pass. Same snapshot, 90 s
   old: 12–23 ms with the virtual clock, 5.7–7.7 s without. This is the "pausing freezes the guest clock" rule from the
   library contract, and it is now needed for correctness, not just tidiness.

### Files
- `basilisk/Dockerfile` (emsdk + gmp/mpfr), `basilisk/build.sh` (bootstrap + base + full Asyncify),
  `basilisk/relink.sh <name> <flags>` (relink only), `basilisk/only.json` (the 3-function list).
- `web/emu-worker.mjs`: the shared host (both emulators), with the nested-loop guard and the virtual clock.
- `web/index.html?emu=basilisk&mode=boot|make|restore&build=…` (`&noclockfix` to reproduce the slowdown, `&wait=ms`).

## Part 3 · SheepShaver (2026-09-29)

Emulator: SheepShaver from Infinite Mac's macemu fork, built exactly as Infinite Mac ships it (-O3, no JIT).
Machine: Power Macintosh 9500 ROM, 64 MB guest RAM, 640×480 · Guest: System 7.5.3 (PPC), the only PowerPC system disk
available locally. Mac OS 9.0.4 was tested afterwards; see "Mac OS 9.0.4" below.
Same JS glue as Basilisk II (the `JS/*.cpp` files are symlinks), so the same host works unchanged.

**It works, still with no source change.** Saved, waited 60 s, restored in fresh pages: 5 of 5 identical first frames,
Apple menu and Calculator work.

| | base | Asyncify, full | Asyncify, 4 functions |
|---|---|---|---|
| Asyncify scope | none | everything reaching JS | `main`, `powerpc_cpu::execute(unsigned int)`, `CheckTicks()`, `ReadJSInput(long long)` |
| wasm size | 715,546 | 810,946 (+13%) | 718,964 (+0.5%) |
| Boot, settled, median (n=6, interleaved) | 8.04 s | 9.46 s | **9.34 s (+16%)** |

Snapshot (4-function build, `INITIAL_MEMORY` 128 MB): raw 128 MB, **gzip 6.4 MB, zstd -19 4.8 MB**; save 318 ms;
restore in a fresh page 126–175 ms (worker 40–74 ms); from the gzip file, page start → first frame 272 ms median
(fetch 30 + gunzip 86 + restore 142).

### What is different about SheepShaver

1. **The speed cost is real here (~15%).** SheepShaver polls input from `CheckTicks()`, which is called from inside
   `powerpc_cpu::execute`, the PPC interpreter loop itself. That function is always on the stack, so it must be
   instrumented, and instrumenting the hottest loop costs speed. (`-sASYNCIFY_IGNORE_INDIRECT=1` changes nothing: the
   limited list already ignores the per-instruction indirect calls.) In Basilisk II and Mini vMac the instrumented
   frames are outside the per-instruction path, hence no cost. Ways out, if 15% matters: accept it; or a small source
   patch that makes `execute` return to an outer loop every N instructions, so only the outer loop is instrumented.
2. **Two more paths into JS.** Input is also polled from the idle path (`execute_sheep → idle_wait → ReadJSInput`, reached
   through an indirect call), and the CPU loop re-enters itself (`execute_macos_code`, `interrupt`, `execute_68k`). The host
   guard is now per emulator: the stack must contain the CPU loop exactly once and none of the listed frames
   (`execute_sheep|idle_wait` for SheepShaver, `idle_wait` for Basilisk II, none for Mini vMac). It never had to defer
   at an idle desktop.
3. The virtual clock (Part 2) is used as-is. Snapshots were restored 60 s after saving with no slowdown.

### Mac OS 9.0.4 (New World ROM)

Disk: Infinite Mac's "Mac OS 9.0.4 HD" (200 MB), fetched as 610 non-empty 256 KB chunks from
`https://infinitemac.org/Disk/<hash>.chunk` using the chunk list in their site bundle, assembled into
`images/macos904.dsk`. ROM: `New-World.rom` from the Infinite Mac repo (what their G3 B&W machine uses).
Same 4-function build, `INITIAL_MEMORY` 128 MB, 64 MB guest RAM.

- Cold boot to a settled Finder: ~21–25 s.
- Save: 89 ms. File 128 MB; **gzip 11.4 MB, zstd -19 8.4 MB** (46 disk chunks written).
- Restored 60 s later in fresh pages, 5 of 5: first frame identical (0 px), Apple menu opens, Calculator opens (12 ms)
  and finishes drawing in 0.73–0.83 s, the same as after a cold boot (0.67–0.74 s, mostly the window-open animation).
- Fresh page → first frame: 499 ms from the raw file, 529 ms from gzip (fetch 35 + gunzip 140 + restore 328).
  Inside the worker the restore itself is 58–81 ms. Most of the page-side time is this harness copying the whole
  200 MB base disk into the worker; a lazy chunked disk would remove that.
- The guest clock shows the time of the snapshot (virtual clock); expected.
- Page params: `emu=sheepshaver&rom=newworld.rom&disk=macos904.dsk&detect=menutext&snapname=os9&calc=60,68`.

### Files
- `basilisk/build-sheep.sh` (configure + base + full Asyncify; `ONLY_RELINK=1 RELINK_NAME=… RELINK_FLAGS=…` to relink),
  `basilisk/only-sheep.json`.
- `web/index.html?emu=sheepshaver&…`, same modes as before.
