#!/usr/bin/env bash
# Builds the v86 core with the VMware SVGA II + SVGA3D device (assets/cores/v86-svga/) from upstream v86:
# clone copy/v86 at a fixed commit, apply patches/, build with Docker, copy the outputs and BIOS images, and check
# them against SHA256SUMS. Needs git, Docker, curl and shasum. OUT=<dir> writes somewhere else.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
V86_REPO=https://github.com/copy/v86
V86_COMMIT=5f9a90f2be01243dd0ea4fe014cce12686cf3ced
QEMU_TAG=v11.0.3                      # vgabios-vmware.bin: QEMU's prebuilt SeaVGABIOS for the VMware SVGA II
OUT=${OUT:-$ROOT/assets/cores/v86-svga}
WORK=$HERE/out/v86

if [ ! -d "$WORK/.git" ]; then git clone -q "$V86_REPO" "$WORK"; fi
git -C "$WORK" fetch -q origin "$V86_COMMIT" 2>/dev/null || git -C "$WORK" fetch -q origin
git -C "$WORK" checkout -q -f "$V86_COMMIT"
git -C "$WORK" clean -qfdx
for p in "$HERE"/patches/*.patch; do git -C "$WORK" apply "$p"; done

docker build -q -t webulator-v86-build - < "$HERE/Dockerfile" >/dev/null
docker run --rm -v "$WORK":/src -w /src webulator-v86-build make build/libv86.mjs build/v86.wasm

mkdir -p "$OUT"
cp "$WORK/build/libv86.mjs" "$WORK/build/v86.wasm" "$WORK/bios/seabios.bin" "$WORK/bios/vgabios.bin" "$OUT/"
curl -fsSL "https://gitlab.com/qemu-project/qemu/-/raw/$QEMU_TAG/pc-bios/vgabios-vmware.bin" -o "$OUT/vgabios-vmware.bin"
(cd "$OUT" && shasum -a 256 -c "$HERE/SHA256SUMS")
echo "core: $OUT (profile buildId = sha256 of v86.wasm)"
