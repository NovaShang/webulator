# Builds d3d9.dll (drop next to an application's executable). Run inside the osm-mingw image.
CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function -fno-strict-aliasing
d3d9.dll: d3d9fwd.c hc.h
	$(CC) $(CFLAGS) -shared -s -o $@ $< -Wl,--kill-at -static-libgcc -lgdi32 -luser32
all: d3d9.dll opengl32.dll
clean:
	rm -f d3d9.dll

# d3d8.dll: crosire/d3d8to9 (BSD-2-Clause) at the commit in d3d8to9-commit.txt with d3d8to9-webulator.patch
# (no d3dx9 dialog), built with:
#   i686-w64-mingw32-g++ -O2 -std=c++17 -DD3D8TO9NOLOG -DNDEBUG -shared -s -o d3d8.dll source/*.cpp res/d3d8.def \
#     -ld3d9 -lgdi32 -static -static-libgcc -static-libstdc++ -Wl,--kill-at

opengl32.dll: glfwd.c glstubs.inc hc.h opengl32.def
	$(CC) $(CFLAGS) -Wno-attributes -shared -s -o $@ glfwd.c opengl32.def -Wl,--kill-at -Wl,--enable-stdcall-fixup -static-libgcc -lgdi32 -luser32
