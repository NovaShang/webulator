> Imported from the OS Museum project (2026-09). Paths such as `spikes/…`, `vendor/…` and `images/…` refer to that project; the reusable code now lives in `prototype/` here.

# S0 · v86 vs QEMU on a real Windows 98 image

Date: 2026-09-27 · Host: Apple M2, 16 GB, macOS 27 · Browser: headless Chromium 153 (Playwright)

## What was compared

| Engine | Version | How it ran |
|---|---|---|
| Native QEMU, TCG only | QEMU 11.0.3 (Homebrew), `qemu-system-i386 -cpu pentium3` | x86 translated to ARM64 on the host, no hardware virtualisation |
| v86 | npm `v86` 0.5.462 | in Chromium, `v86.wasm` |
| qemu-wasm | `ktock/qemu-wasm` @ 0ef7b4e, `qemu-system-x86_64 -cpu pentium3`, Wasm TCG backend, single vCPU | in Chromium |

Guest: the Windows 98 disk from the v86 demo (i.copy.sh, 300 MB), 128 MB RAM for Win98 runs.
Each engine got its own copy of the disk with drivers for its emulated hardware installed first
(QEMU: `prep_qemu.py`; v86: `web/prep-v86.html`), so no "Add New Hardware" wizard interrupts a boot.

## Results

### CPU micro-benchmark
The same 512-byte boot floppy on every engine: a fixed 8-instruction ALU + memory loop, timed against
the emulated clock (BIOS ticks in 16-bit mode, CMOS RTC seconds in 32-bit mode). Median of 5 runs,
blocks of 1000 iterations completed in 5 seconds. Higher is better.

| Engine | 32-bit protected mode | vs native QEMU | 16-bit real mode | vs native QEMU |
|---|---|---|---|---|
| Native QEMU (TCG) | 1,413,264 | 1.00× | 1,389,672 | 1.00× |
| **v86** | **718,185** | **0.51×** | 536,400 | 0.39× |
| qemu-wasm | 353,178 | 0.25× | 379,309 | 0.27× |

### Windows 98
Seconds, median. Lower is better. Native QEMU and v86: 5 runs. qemu-wasm: 3 good runs, 1 failed
(monitor timeout, see caveats).

| Engine | Boot to desktop | Boot settled | Start menu opens | Notepad opens | Snapshot save | Snapshot restore |
|---|---|---|---|---|---|---|
| Native QEMU (TCG) | 13.71 | 16.26 | 0.51 | 0.43 | 0.06 | 0.04 |
| **v86** | **13.89** | **15.61** | **0.22** | **0.13** | 0.05 | 0.04 |
| qemu-wasm | 32.95 | 40.01 | 1.75 | 1.74 | 0.21 | 0.29 |

Snapshot size: v86 state 37.4–37.7 MB uncompressed; QEMU `savevm` reports 26.8 MB.

### Download size of the engine

| | Raw | gzip |
|---|---|---|
| v86 (`v86.wasm` + `libv86.js` 0.36 MB) | 2.1 MB | 0.4 MB |
| qemu-wasm (`qemu-system-x86_64.wasm`, built with `-g`) | 41.9 MB | 14.8 MB |

## What this says

1. **For x86 guests in the browser, v86 is clearly ahead of qemu-wasm today.** It runs the CPU loop about
   2× faster, boots Win98 2.4× faster, and opens apps about 10× faster, from a download that is 35× smaller.
2. **v86 reaches about half the speed of native QEMU-TCG on raw CPU work, and matches it on Win98 boot.**
   Boot is dominated by disk and timers, not CPU. Its UI numbers even beat native QEMU, partly because
   reading v86's canvas is cheaper than a QMP `screendump` (see caveats).
3. **Snapshot restore takes about 40 ms in-process on both v86 and native QEMU.** Once the state is in memory,
   waking an exhibit is effectively instant. The real cost is fetching a ~37 MB state (before compression),
   which is what S1 and S3 need to attack.
4. **qemu-wasm is not ready to be our x86 engine.** Getting a picture out of it took two rebuilds.
   The README configuration disables pixman, so `screendump` does not exist, and there is no display
   backend at all: we read the screen through the HMP monitor. Keep it on the research track for what
   v86 cannot do (x86-64, and possibly PowerPC).

## Caveats

- **qemu-wasm UI timings are inflated.** Its screen can only be read with a monitor `screendump` that writes
  a 900 KB PPM through the proxied Emscripten filesystem, and keys go in through `sendkey`. The CPU
  benchmark (read from the serial port) is not affected. One of four Win98 runs failed with a monitor timeout.
- Native QEMU here is **cross-architecture** (x86 on ARM64). On an x86 host, native TCG would be faster still.
- 16-bit results are noisier on v86 and are less relevant: Win9x spends most of its time in 32-bit code.
- An earlier v86 CPU run driven through the Chrome extension read about 5× lower (138k blocks). That session had
  a debugger attached and a busy machine, so it is discarded. All numbers above come from an idle machine.
- One image, one host, one browser. Safari and Firefox, and a weaker laptop, are still to do (S1).

## Reproduce

```sh
# images: see ../../images (downloaded from i.copy.sh); build qemu-wasm: ./build-qemu-wasm.sh
./bench_native_cpu.sh > results/native_qemu_cpu.txt
python3 prep_qemu.py && python3 bench_native.py 5
cd web && node serve.mjs . 8766 &          # Range + COOP/COEP
node run-page.mjs "http://localhost:8766/prep-v86.html" ../results/v86_prep.json 2400   # once
cd .. && ./run_browser_all.sh && python3 summarize.py
```

Local changes needed to build qemu-wasm (in `qemu-wasm/` and `build-qemu-wasm.sh`): zlib download URL moved
to the GitHub release, the source mount must be writable (meson fetches subprojects), and `--enable-pixman`.
