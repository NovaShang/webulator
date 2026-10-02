# Conformance results

Run on 2026-09-30 (T1–T11) and 2026-10-02 (T12, and T1–T11 again with the patched Mac cores) · Apple M2 ·
headless Chromium 153 (Playwright 1.63) · `node conformance/run.mjs`

**All four profiles pass all twelve tests.**

| Test | What it checks | Mini vMac · System 6.0.8 | Basilisk II · System 7.5.3 | SheepShaver · Mac OS 9.0.4 | v86 · Windows 98 |
|---|---|---|---|---|---|
| T1 | Cold boot to a settled desktop | PASS · 2.7 s | PASS · 4.0 s | PASS · 27.9 s | PASS · 19.1 s |
| T2 | Save → destroy → restore into a new worker, first frame identical, ×5 | PASS · 0 px ×5 · 46–85 ms | PASS · 0 px ×5 · 77–140 ms | PASS · 0 px ×5 · 158–217 ms | PASS · 0 px ×5 · 137–170 ms |
| T3 | Alive after restore (menu opens, an app opens) | PASS | PASS | PASS | PASS |
| T4 | Restored after a ≥ 60 s gap, responds like a fresh restore | PASS | PASS | PASS | PASS |
| T5 | Pause 10 s: guest clock advance, frames while paused | PASS · 0 ms, 0 frames | PASS · 0 ms, 0 frames | PASS · 0 ms, 0 frames | PASS · 0 ms, 0 frames |
| T6 | `moveTo` 20 random points, cursor lands exactly | PASS · 20/20 exact | PASS · 20/20 exact | PASS · 20/20 exact | PASS · 20/20 exact |
| T7 | `type()` all 95 printable ASCII characters, compared with a golden image | PASS · 0 px | PASS · 0 px | PASS · 0 px | PASS · 0 px |
| T8 | Disk overlay exported and booted in a new machine | PASS · 16 KB | PASS · 80 KB | PASS · 192 KB | PASS · 2.9 MB |
| T9 | Snapshot requested while the guest is busy booting, then restored | PASS · 164 ms | PASS · 269 ms | PASS · 497 ms | PASS · 270 ms |
| T10 | No input lost (the ~380 key events of T7 sent back to back) | PASS | PASS | PASS | PASS |
| T11 | T1–T3 with no display attached | PASS | PASS | PASS | PASS |
| T12 | Disk fetches (each delayed 400 ms) do not stall the guest: longest gap between frames while the pointer moves | PASS · 4 fetches · 33 ms | PASS · 18 fetches · 37 ms | PASS · 28 fetches · 59 ms | PASS · 16 fetches · 36 ms |

Snapshot of the settled desktop (container, gzip sections): 0.57 MB · 2.0 MB · 11.1 MB · 12.4 MB.

## How the tests decide

- **Settled** means fewer than 50 pixels change for the required time. A blinking text caret passes; anything larger
  does not. Frame events cannot be used for this, because Basilisk II and SheepShaver report whole-frame damage.
- **T6** finds the cursor by diffing a cursor-free frame with one after `moveTo`, over a solid-colour area. The diff
  box starts at a fixed offset from the hot spot for a given cursor shape (the I-beam over text: −3, −4; the Mac OS 9
  arrow on the desktop: −1, −1; the Windows arrow: 0, 0). Each profile declares that offset once; the test requires
  all 20 points to match it. A scaling or rounding error would make the error depend on position.
- **T7** compares only pixels that stay still over 1.5 s, so a blinking caret is ignored. Golden images are recorded
  with `--record`, checked by eye, and kept in `conformance/golden/` (not committed: they are screenshots of the
  guest systems).
- **T12** restores the desktop snapshot into a worker whose disk cache is empty, with every chunk fetch delayed by
  400 ms, moves the pointer in a circle at 60 Hz and runs the profile's `diskLoad` (Mac: launch four Apple-menu items;
  Windows: open Notepad). The sweep pauses while the hook itself uses the pointer or waits for the screen, and only
  frame gaps inside sweeping time count. It passes with at least 3 fetches and no gap of 200 ms or more. Before the
  disk patches the three Mac cores failed it with gaps of 416–836 ms: the guest stood still for each fetch.
- **T8** shuts the guest down first where the core supports guest power-off (Mini vMac, Basilisk II, v86). SheepShaver
  cannot (see below), so its test boots the overlay of a machine that was not shut down, and the hook presses Return
  to close Mac OS 9's "did not shut down properly" notice.

## Profile preparation

- **Windows 98** (`profiles/v86-win98.json`) uses the v86 demo image with two changes, made on the host and then
  "baked":
  1. VBMOUSE from [VBADOS](https://git.javispedro.com/cgit/vbados.git/about/) installed with mtools
     (`C:\VBADOS\VBMOUSE.EXE` in `AUTOEXEC.BAT`, `mouse.drv=vbmouse.drv` in `SYSTEM.INI`) for the absolute pointer.
  2. One boot with `--suite=bake`: Windows reconfigures for the new driver and restarts, then shuts down cleanly;
     the exported overlay is merged into the base image with `tools/apply-overlay.mjs`.
- The image waits for a DHCP lease at boot (it has networking installed). The profile sets `offlineNic`: an NE2000
  card whose ARP/DHCP are answered inside the worker, with `fetch` closed after start-up so no guest traffic leaves.
  Without it the boot takes about 15 s longer.

## Pointer responsiveness

Measured 2026-10-01 in headless Chromium, restored desktop snapshots, pointer moving with no button held
(the case reported as stuttering). "Latency" is the time from `moveTo` until the cursor shows at the target.

| Profile | Frames/s while moving | Longest frame gap | Latency, median / p90 |
|---|---|---|---|
| Mini vMac · System 6.0.8 | 60 | 18 ms | 10 / 15 ms |
| Basilisk II · System 7.5.3 | 60 | 22 ms | 30 / 37 ms |
| SheepShaver · Mac OS 9.0.4 | 46 (was 7) | 41 ms (was 174) | 35 / 45 ms (was 303 / 402) |
| v86 · Windows 98 | 52 | 36 ms | 24 / 33 ms |

Two causes were fixed:
- **SheepShaver** checks its 60 Hz tick (which drives screen refresh and the cursor) only every 50 000 instructions.
  When the guest idled, the worker blocked up to 8 ms per idle call, so ticks came every ~160 ms. Idle waits now end
  when the next refresh is due (as Infinite Mac does). Dragging was smooth because the guest is busy then.
- **All cores**: frames were posted at most every 16 ms; a 60 Hz guest refresh that came a little early was held
  back to the next one. The floor is now 12 ms.

v86 cannot render more often than every 16 ms: rendering also sets its vertical-retrace bit, and Windows 98 hung at
its boot logo in 3 of 8 cold boots with a 14 ms threshold. Its frames therefore arrive every 16–20 ms (an occasional
repeated frame).

## Known issues

- **SheepShaver: guest Shut Down traps the core** (a wasm out-of-bounds access in the PPC interpreter). The same
  happens with Infinite Mac's own 288 MB build, so it is a core issue, not the adapter's. The machine reports
  `crashed`. Basilisk II and Mini vMac power off cleanly and the machine stays usable for overlay export.
- **Windows 98 with VBMOUSE: about 1 cold boot in 12 hangs at the boot logo.** At the hang the IDE has finished a
  transfer whose interrupt was never delivered. The same runtime boots the image without VBMOUSE 12 of 12 times, so
  the likely cause is a race in the guest while the VBMOUSE TSR initialises (an early PS/2 mouse interrupt left
  unacknowledged blocks the lower-priority disk interrupt). Restoring from a snapshot is not affected. Running v86's
  scheduling inside the worker (below) cut the rate from about 1 in 4.
- v86 yields through `scheduler.postTask` inside the worker (the worker URL carries `use-scheduling-api`). Its
  default nested-worker round trip slowed down when the page's main thread was busy, guest time ran on while the
  CPU stood still, and cold boots failed far more often.
- Basilisk II pauses 130 ms after `pause()` in some runs (the request is seen at the next input poll); the clock is
  still frozen for the whole pause.
- v86 shows the 320×400 Windows 98 boot logo at its native size; the API reports the guest mode as is.

## Reproduce

```sh
npm install && npm run build
node conformance/run.mjs                       # all profiles
node conformance/run.mjs v86-win98 --tests=T6,T7
node conformance/run.mjs <profile> --suite=smoke   # boot 60 s, screenshots, disk access log
node conformance/run.mjs <profile> --suite=explore "--steps=down:185,8;shot:menu;up:185,8"
```

Cores, ROMs and disk images go in `assets/` (not committed); see `profiles/*.json` for the expected paths and
`tools/make-manifest.mjs` for turning an image into chunks.
