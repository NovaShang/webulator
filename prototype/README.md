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

## Assets you supply

ROMs (Mac Plus, Quadra 650, Power Macintosh 9500, New World) and system disk images are not in this repository. The
prototype was run with the files that the Infinite Mac project uses.
