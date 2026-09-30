#!/bin/bash
# Runs inside emscripten/emsdk:4.0.22. Builds Mini vMac (Mac Plus, all-out speed) twice from the same objects:
# base = Infinite Mac's link flags at -O3; async = the same plus Asyncify limited to the main-loop path.
set -e
cd /src/minivmac
gcc -o setup_t setup/tool.c
./setup_t -t emsc -api esc -sound 1 -drives 20 -sony-tag 1 -sony-sum 1 -sony-dc42 1 -d ${DBG:-d} -speed a > setup.sh
bash ./setup.sh >/dev/null
sed -i "s/-O[0-3s]\\b/${OPT:--O0}/g" Makefile
grep -m1 mk_COptions Makefile
make clean >/dev/null; make -j8 bld/MINEM68K.o bld/OSGLUESC.o bld/GLOBGLUE.o bld/M68KITAB.o bld/VIAEMDEV.o bld/IWMEMDEV.o bld/SCCEMDEV.o bld/RTCEMDEV.o bld/ROMEMDEV.o bld/SCSIEMDV.o bld/SONYEMDV.o bld/SCRNEMDV.o bld/MOUSEMDV.o bld/KBRDEMDV.o bld/SNDEMDEV.o bld/PROGMAIN.o >/dev/null
OBJS=$(ls bld/*.o)
COMMON="${OPT:--O0} -g2 -s INITIAL_MEMORY=50331648 -s MODULARIZE -s EXPORT_ES6 -s EXPORT_NAME=emulator \
  -s EXPORTED_RUNTIME_METHODS=[\"FS\",\"HEAPU8\"] -s EXPORTED_FUNCTIONS=[\"_malloc\",\"_free\",\"_main\"] \
  -s ENVIRONMENT=worker -flto --pre-js /src/post.js"
mkdir -p /src/out
emcc $OBJS $COMMON -o /src/out/base${SUF}.mjs
ONLY='["main","__main_void","__main_argc_argv","__original_main","ProgramMain","MainEventLoop","WaitForNextTick","ReadJSInput"]'
emcc $OBJS $COMMON -s ASYNCIFY=1 -s "ASYNCIFY_IMPORTS=[\"env.emscripten_asm_const_int\"]" -s "ASYNCIFY_ONLY=$ONLY" \
  -s ASYNCIFY_STACK_SIZE=65536 -o /src/out/async${SUF}.mjs
ls -la /src/out
