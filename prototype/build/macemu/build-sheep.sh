#!/bin/bash
# Inside osm-basilisk-build: build SheepShaver the way Infinite Mac does, then link base + full Asyncify.
set -e
source /emsdk/emsdk_env.sh >/dev/null 2>&1
cd /src/macemu/SheepShaver/src/Unix
if [ ! -f .configured ]; then
  make clean >/dev/null 2>&1 || true
  ./_emconfigure.sh > /src/sheep-configure.log 2>&1
  echo 'LDFLAGS += $(EXTRA_LDFLAGS)' >> Makefile
  touch .configured
fi
link() {
  name=$1; shift
  rm -f SheepShaver SheepShaver.js SheepShaver.wasm
  make -j8 EXTRA_LDFLAGS="--pre-js /src/post.js $*" > /src/make-sheep-$name.log 2>&1 || { tail -40 /src/make-sheep-$name.log; exit 1; }
  js=SheepShaver; [ -f SheepShaver.js ] && js=SheepShaver.js
  cp $js /src/out/sheep-$name.mjs; cp SheepShaver.wasm /src/out/sheep-$name.wasm
  sed -i "s/SheepShaver\.wasm/sheep-$name.wasm/g" /src/out/sheep-$name.mjs
}
[ -z "$ONLY_RELINK" ] && link base
[ -z "$ONLY_RELINK" ] && link async '-sASYNCIFY=1 -sASYNCIFY_IMPORTS=["env.emscripten_asm_const_int","env.emscripten_asm_const_double"]'
/bin/ls -la /src/out/sheep-*
[ -n "$RELINK_NAME" ] && link $RELINK_NAME $RELINK_FLAGS
