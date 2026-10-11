#!/bin/sh
# build_switch.sh - builds rampage.nro, RampageRE's native viewer (src/) for a Nintendo Switch with homebrew (custom
# firmware such as Atmosphere), the WoodyRE way: one C compile. Copy rampage.nro to /switch/rampagere/ on the SD card,
# with your own disc image (or RAMPAGE.VOL and the ELF) beside it, and start it from the Homebrew Menu (docs/NATIVE.md
# "Nintendo Switch"). Needs devkitPro's devkitA64 with libnx, switch-sdl2 and switch-mesa (devkitpro.org); without
# $DEVKITPRO it builds in the official devkitpro/devkita64 Docker image instead, which has all of that.
# The SRC list is build_viewer.sh's (keep them in step) with src/gles/gles2.c: OpenGL ES through the shim, as GLES=1.
set -e
cd "$(dirname "$0")"
VERSION=${RAMPAGE_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
if [ -z "$DEVKITPRO" ]; then
    echo "DEVKITPRO is not set: building in the devkitpro/devkita64 Docker image ..."
    exec docker run --rm -e RAMPAGE_VERSION="$VERSION" -v "$PWD":/src -w /src devkitpro/devkita64 sh ./build_switch.sh "$@"
fi
OUT=${1:-rampage}
VERSION=$(printf '%.15s' "${VERSION#v}")                 # the NACP's: at most 15 characters
PORT=$DEVKITPRO/portlibs/switch
PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
SRC="src/main.c src/shot.c src/render_gl.c src/city.c src/vol.c src/bundle.c src/tex.c src/mesh.c src/prb.c
     src/tree.c src/anim.c src/camera.c src/moves.c src/building.c src/climb.c src/ik.c src/people.c src/fx.c
     src/cloth.c src/sound.c src/army.c src/hud.c src/vehicle.c src/water.c src/edible.c src/game.c src/boss.c
     src/look.c src/shadow.c src/director.c src/footstep.c src/challenge.c src/reveal.c src/rail.c src/parade.c
     src/streetobj.c src/profile.c src/collide.c src/shell.c src/movie.c src/wrand.c src/texpack.c src/iso.c
     src/plat_sdl.c src/pad_sdl.c src/settings.c src/postfx.c src/gtao.c src/gles/gles2.c"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
# src/gles first on the include path: its GL/gl.h puts the renderer's fixed-function OpenGL on OpenGL ES 3.0 / 2.0;
# char is unsigned on ARM, the viewer (like the game's data) takes it as signed, as on x86
CFLAGS="-std=gnu99 -O2 -ffunction-sections -fsigned-char -Wall -Wno-format-truncation $ARCH -D__SWITCH__
        -Isrc/gles -Isrc -I$PORT/include -I$PORT/include/SDL2 -I$DEVKITPRO/libnx/include"
LIBS="-L$PORT/lib -L$DEVKITPRO/libnx/lib -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lstdc++ -lnx -lm"
mkdir -p build/switch
echo "Building $OUT.nro (RampageRE $VERSION) ..."
# shellcheck disable=SC2086
aarch64-none-elf-gcc $CFLAGS -specs="$DEVKITPRO/libnx/switch.specs" -Wl,-Map,build/switch/$OUT.map -o build/switch/$OUT.elf $SRC $LIBS
nacptool --create "RampageRE" "jjmhalew" "$VERSION" build/switch/$OUT.nacp
elf2nro build/switch/$OUT.elf "$OUT.nro" --icon=res/switch_icon.jpg --nacp=build/switch/$OUT.nacp
echo "Done: $OUT.nro"
