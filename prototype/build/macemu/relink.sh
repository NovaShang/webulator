#!/bin/bash
# Relink Basilisk II (objects already built by build.sh) with extra flags: relink.sh <name> <flags...>
set -e
source /emsdk/emsdk_env.sh >/dev/null 2>&1
cd /src/macemu/BasiliskII/src/Unix
name=$1; shift
rm -f BasiliskII BasiliskII.js BasiliskII.wasm
make -j8 EXTRA_LDFLAGS="--pre-js /src/post.js $*" > /src/make-$name.log 2>&1 || { tail -30 /src/make-$name.log; exit 1; }
cp BasiliskII /src/out/$name.mjs; cp BasiliskII.wasm /src/out/$name.wasm
sed -i "s/BasiliskII\.wasm/$name.wasm/g" /src/out/$name.mjs
grep -i "warning" /src/make-$name.log | head -5
/bin/ls -la /src/out/$name.*
