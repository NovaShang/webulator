#!/bin/bash
# Inside osm-basilisk-build. Builds Basilisk II the way Infinite Mac does, then relinks it twice:
# base (unchanged flags) and async (+ Asyncify on the path from main() to the input poll).
set -e
source /emsdk/emsdk_env.sh
cd /src/macemu/BasiliskII/src/Unix
if [ ! -f .bootstrapped ]; then
  make clean >/dev/null 2>&1 || true
  ./_embootstrap.sh > /src/bootstrap.log 2>&1
  ./_emconfigure.sh > /src/configure.log 2>&1
  touch .bootstrapped
fi
echo 'LDFLAGS += $(EXTRA_LDFLAGS)' >> Makefile
mkdir -p /src/out
rm -f BasiliskII BasiliskII.js BasiliskII.wasm
make -j8 EXTRA_LDFLAGS="--pre-js /src/post.js" > /src/make-base.log 2>&1 || { tail -30 /src/make-base.log; exit 1; }
cp BasiliskII.js /src/out/base.mjs 2>/dev/null || cp BasiliskII /src/out/base.mjs; cp BasiliskII.wasm /src/out/base.wasm
sed -i 's/base\.wasm/base.wasm/' /src/out/base.mjs
rm -f BasiliskII BasiliskII.js BasiliskII.wasm
make -j8 EXTRA_LDFLAGS="--pre-js /src/post.js -sASYNCIFY=1 -sASYNCIFY_IMPORTS=[\"env.emscripten_asm_const_int\",\"env.emscripten_asm_const_double\"] ${ASYNC_EXTRA}" > /src/make-async.log 2>&1 || { tail -30 /src/make-async.log; exit 1; }
cp BasiliskII.js /src/out/async.mjs 2>/dev/null || cp BasiliskII /src/out/async.mjs; cp BasiliskII.wasm /src/out/async.wasm
/bin/ls -la /src/out
