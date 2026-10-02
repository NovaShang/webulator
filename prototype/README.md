# Prototype

The code behind the Mac snapshot measurements in `docs/findings/mac-snapshots.md`. It is a spike, not the library:
the spec's input ring buffer, block-device laziness and v86 support are not here yet.

| File | What |
|---|---|
| `web/emu-worker.mjs` | Shared worker runtime for Infinite Mac's emulator builds: our own `workerApi`, Asyncify snapshot and restore, the per-core nesting guard, the virtual clock, 4 KB disk write tracking |
| `web/index.html` | Test page: `?emu=minivmac|basilisk|sheepshaver&mode=boot|make|restore&build=…` |
| `web/serve.mjs` | Static server with Range, COOP/COEP and `PUT /uploads/` |
| `web/run-page.mjs` | Playwright runner: `node run-page.mjs <url> <out.json> [timeout_s]` |
| `build/minivmac/build.sh` | Builds Mini vMac (Mac Plus) twice: base and Asyncify |
| `build/macemu/` | Dockerfile (emsdk 4.0.22 + gmp/mpfr), Basilisk II and SheepShaver builds, relink script, `ASYNCIFY_ONLY` lists |
| `build/*/post.js` | The `--pre-js` that exposes Asyncify, the exports and the stack helpers |

## Upstream sources (not included)

| Emulator | Repository | Commit |
|---|---|---|
| Basilisk II, SheepShaver | https://github.com/mihaip/macemu | `f44bedce698ff5bf600185a7b15c904b9c43758a` |
| Mini vMac | https://github.com/mihaip/minivmac | `d57bcead53b68867b4c6aa1f3e4915ac669361f4` |
| v86 | https://github.com/copy/v86 (npm `v86` 0.5.462) | `5f9a90f2be01243dd0ea4fe014cce12686cf3ced` |

The build scripts expect a copy of the emulator source mounted at `/src/macemu` or `/src/minivmac` inside the container.

## Patches

The cores in use carry one patch each, applied to the commits above with `patch -p1`:

| Patch | What |
|---|---|
| `build/macemu/patches/async-disk.patch` | Basilisk II and SheepShaver: the disk driver's Prime reports "busy" while the runtime is still fetching the data (`workerApi.disks.ready`), and the driver stub in ROM retries, so the guest keeps running (and drawing its cursor) during the fetch |
| `build/minivmac/patches/async-disk.patch` | Mini vMac: the same, with the retry loop appended after the replacement `.Sony` driver |

Production links: Basilisk II with `relink.sh` and `-sASYNCIFY_ONLY=@/src/only.json -sINITIAL_MEMORY=83886080`;
SheepShaver with `build-sheep.sh` (`RELINK_FLAGS` with `only-sheep.json`, `-sINITIAL_MEMORY=134217728`); Mini vMac
with `build.sh` (the `async` output, `-O0`).

## Assets you supply

ROMs (Mac Plus, Quadra 650, Power Macintosh 9500, New World) and system disk images are not in this repository. The
prototype was run with the files that the Infinite Mac project uses.
