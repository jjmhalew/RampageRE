#!/bin/sh
# build_viewer.sh - builds ./rampage, RampageRE's native viewer (src/), for Linux with SDL2 (src/plat_sdl.c), the WoodyRE
# way: one C compile. Needs a C compiler, SDL2 and OpenGL headers, e.g. on Debian / Ubuntu:
#   sudo apt install build-essential libsdl2-dev libgl-dev
# (Windows: build_viewer.bat; the SRC list is the same there, without plat_sdl.c and with pad.c for pad_sdl.c.) Run it
# from the repository root:
# ./rampage [RAMPAGE.VOL] [City] [block]; with no display: SDL_VIDEODRIVER=offscreen ./rampage ... --shot s.png
# GLES=1 ./build_viewer.sh draws through OpenGL ES 3.0 / 2.0 instead (src/gles: the fixed function emulated on it, the path
# a Switch or Android build takes), to test that path on a PC; it needs libgles-dev instead of libgl-dev.
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
OUT=${1:-rampage}
SRC="src/main.c src/shot.c src/render_gl.c src/city.c src/vol.c src/bundle.c src/tex.c src/mesh.c src/prb.c
     src/tree.c src/anim.c src/camera.c src/moves.c src/building.c src/climb.c src/ik.c src/people.c src/fx.c
     src/cloth.c src/sound.c src/army.c src/hud.c src/vehicle.c src/water.c src/edible.c src/game.c src/boss.c
     src/look.c src/shadow.c src/director.c src/footstep.c src/challenge.c src/reveal.c src/rail.c src/parade.c
     src/streetobj.c src/profile.c src/collide.c src/shell.c src/movie.c src/wrand.c src/texpack.c src/iso.c
     src/plat_sdl.c src/pad_sdl.c src/settings.c src/postfx.c src/gtao.c"
GL="-lGL"
if [ "$GLES" = 1 ]; then SRC="$SRC src/gles/gles2.c"; GL="-Isrc/gles -fsigned-char -lGLESv2"; fi   # (char is unsigned on ARM)
echo "Building $OUT ..."
# -Wno-format-truncation: gcc warns of every snprintf that may cut its text, which is what they are there for
# shellcheck disable=SC2086
$CC -std=gnu99 -O2 -Wall -Wno-format-truncation -D_FILE_OFFSET_BITS=64 -o "$OUT" $SRC $(sdl2-config --cflags) $(sdl2-config --libs) $GL -lm
echo "Done: $OUT"
