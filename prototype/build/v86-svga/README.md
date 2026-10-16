# v86 with VMware SVGA II + SVGA3D (`assets/cores/v86-svga/`)

The core behind the "Windows XP [3D]" demo: upstream v86 plus an emulated VMware SVGA II adapter whose 3D
commands (SVGA3D, the legacy Direct3D 9–level protocol) are rendered with WebGL2 in the emulator's worker. Guests
reach it two ways:

- **VMware's own guest driver** (VMware Tools 10.0.12 for Windows XP: `vmx_svga.sys`, `vmx_fb.dll`, the
  `vmwogl32.dll` OpenGL ICD) talks to the device through the FIFO, as on real VMware. This works but is slow:
  Mesa's OpenGL driver and, for Direct3D 8/9, WineD3D run on the emulated CPU.
- **API forwarding** (`guest/`, our own runtimes): `d3d9.dll` and `opengl32.dll` placed next to an application
  turn Direct3D 9 / OpenGL 1.x calls into SVGA3D commands in the application's own process and hand them over with
  a user-mode hypercall (`out 0x5680, eax`). Vertex, index and texel data is passed by guest linear address and
  read through the guest page tables, so the guest copies nothing. Direct3D 8 goes through d3d8to9 into
  `d3d9.dll`. The demo's OpenArena uses the forwarded `opengl32.dll`.

## Build

```sh
prototype/build/v86-svga/build.sh            # writes assets/cores/v86-svga/
OUT=/tmp/core prototype/build/v86-svga/build.sh
```

It clones [copy/v86](https://github.com/copy/v86) at `5f9a90f2be01243dd0ea4fe014cce12686cf3ced`, applies
`patches/*.patch`, builds `libv86.mjs` and `v86.wasm` in Docker (`Dockerfile`: Rust 1.88 + wasm32, clang, Java for
Closure Compiler, Node), copies SeaBIOS and VGA BIOS from the v86 tree and `vgabios-vmware.bin` from QEMU v11.0.3,
and checks all five files against `SHA256SUMS`. The build is reproducible: a clean build on 2026-10-16 matched the
deployed core byte for byte. The profile's `core.buildId` is the SHA-256 of `v86.wasm`
(`profiles/v86-winxp.json`: `sha256:bbaa4ecc…`).

The device is enabled per machine: `coreConfig.options.vmware_svga: true` and the `vgabios-vmware.bin` VGA BIOS
(its PCI ID must match the emulated card, 15AD:0405). Other v86 profiles keep using `assets/cores/v86/`.

## What the patch changes

| file | |
|---|---|
| `src/vmware_svga.js` | the VMware SVGA II device: registers, FIFO, GMR/GMR2, 3D capabilities, the hypercall port and the linear-address pseudo-GMR |
| `src/vmware_svga3d.js` | SVGA3D on WebGL2: surfaces and formats (incl. DXT), contexts, clears, fixed-function transform, lighting and texture combiners, shader path, streamed buffers, presents (copied to a front texture, read back into VRAM once per screen refresh) |
| `src/vmware_svga3d_shader.js` | Direct3D 9 shader bytecode (SM 2/3) → GLSL ES 3.00 |
| `src/vmware.js` | VMware backdoor GuestRPC (the guest driver's log; answers `guestinfo.svga.enableOpenGL`) |
| `src/vga.js`, `src/cpu.js`, `src/browser/starter.js`, `src/main.js`, `Makefile` | wiring, the `vmware_svga` option, tick statistics |
| `src/rust/cpu/cpu.rs`, `src/rust/js_api.rs` | user-mode access to the hypercall port; side-effect-free address translation and JIT invalidation for JS |
| `src/rust/softfloat.rs` | x87 fast path: with round-to-nearest and 53- or 24-bit precision, operations on doubles use native f64 (bit-identical to SoftFloat over 168 M random cases) |

## Guest runtimes (`guest/`)

Built with mingw-w64 (`i686-w64-mingw32-gcc`, see `Makefile`): `d3d9.dll` from `d3d9fwd.c`, `opengl32.dll` from
`glfwd.c` (`glstubs.inc` and `opengl32.def` are generated from mingw-w64's `GL/gl.h`: every OpenGL 1.1 export
must exist with the right stack size). `hc.h` is the shared hypercall transport. `d3d8.dll` is
[crosire/d3d8to9](https://github.com/crosire/d3d8to9) at the commit in `d3d8to9-commit.txt` with
`d3d8to9-webulator.patch` (removes a dialog about the missing `d3dx9_43.dll`). `glarrays.c` draws twelve
vertex-array setups side by side, to compare OpenGL runtimes.

## Licences

- v86 and therefore `patches/`: BSD-2-Clause (copyright the v86 contributors; the added files are under the same
  terms).
- `seabios.bin`, `vgabios.bin` (from v86's `bios/`) and `vgabios-vmware.bin` (QEMU's build of SeaVGABIOS):
  LGPL-3.0, sources at https://www.seabios.org.
- `guest/`: MIT like this repository, except `d3d8to9-webulator.patch` (BSD-2-Clause, d3d8to9).
- Not here and not redistributable by us: Windows XP, VMware Tools (proprietary; the demo image contains it).

## Known limits

- Rendering waits on the guest: OpenArena's timedemo runs at about 17 fps, bound by its own x87 maths (MinGW
  build, 80-bit precision, which the f64 fast path cannot take). Irrlicht's Quake 3 map: 329 FPS forwarded
  OpenGL, 498 FPS forwarded Direct3D 9, 158 FPS with VMware's Mesa driver.
- The 3D frame is drawn into VRAM over the window's area: windows on top of a 3D window are painted over.
- SVGA3D: no cube or volume textures, no user clip planes, no vertex blending; texture transforms and texgen are
  ignored by the fixed-function path; one render target.
- `d3d9.dll`: no cube/volume textures, `ProcessVertices`, patches or swap chains beyond the implicit one;
  shader model 2.0 caps. D3D8 applications with shaders need `d3dx9_43.dll` next to them (d3d8to9 assembles with it).
- `opengl32.dll`: OpenGL 1.5 without GLSL; no texgen, texture matrices, clip planes, display lists (compiled
  lists are dropped), `glCopyTexImage`, cube maps or framebuffer objects.
- Needs WebGL2 (and `WEBGL_compressed_texture_s3tc` for DXT textures).
