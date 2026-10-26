#!/bin/sh
# build_vita.sh - builds rampage.vpk, RampageRE's native viewer (src/) for a PS Vita (or PS TV) with homebrew enabled
# (HENkaku / Ensō), the Switch build's way: one C compile. Install rampage.vpk with VitaShell, put your own disc image
# (or RAMPAGE.VOL and the ELF) in ux0:data/rampagere/ and start it from LiveArea (docs/NATIVE.md "PS Vita"). Needs
# VitaSDK with its SDL2 and vitaGL (vitasdk.org); without $VITASDK it builds in the official vitasdk/vitasdk Docker
# image instead, which has all of that.
# The SRC list is build.sh's (keep them in step) with src/gles/gles2.c: OpenGL ES 2.0 through the shim, as GLES=1, on
# vitaGL (VitaSDK's SDL2 makes its GL contexts with it).
set -e
cd "$(dirname "$0")"
VERSION=${RAMPAGE_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
if [ -z "$VITASDK" ]; then
    echo "VITASDK is not set: building in the vitasdk/vitasdk Docker image ..."
    export MSYS_NO_PATHCONV=1                       # (Git Bash on Windows: /src stays /src)
    exec docker run --rm -e RAMPAGE_VERSION="$VERSION" -v "$PWD":/src -w /src vitasdk/vitasdk sh ./build_vita.sh "$@"
fi
OUT=${1:-rampage}
APP_VER=00.00                                       # param.sfo's XX.YY: v1.1.1 is 01.11, anything else (a commit) 00.00
case ${VERSION#v} in
[0-9].[0-9].[0-9] | [0-9][0-9].[0-9].[0-9]) APP_VER=$(echo "${VERSION#v}" | awk -F. '{ printf "%02d.%d%d", $1, $2, $3 }') ;;
esac
TITLE_ID=RMPR00001                                  # four letters, five digits: the folder ux0:app/RMPR00001
PATH=$VITASDK/bin:$PATH
SRC="src/main.c src/shot.c src/render_gl.c src/city.c src/vol.c src/bundle.c src/tex.c src/mesh.c src/prb.c
     src/tree.c src/anim.c src/camera.c src/moves.c src/building.c src/climb.c src/ik.c src/people.c src/fx.c
     src/cloth.c src/sound.c src/army.c src/hud.c src/vehicle.c src/water.c src/edible.c src/game.c src/boss.c
     src/look.c src/shadow.c src/director.c src/footstep.c src/challenge.c src/reveal.c src/rail.c src/parade.c
     src/streetobj.c src/profile.c src/collide.c src/shell.c src/movie.c src/wrand.c src/texpack.c src/iso.c
     src/plat_sdl.c src/pad_sdl.c src/settings.c src/postfx.c src/gtao.c src/autoplay.c src/gles/gles2.c"
# src/gles first on the include path: its GL/gl.h puts the renderer's fixed-function OpenGL on OpenGL ES 2.0; char is
# unsigned on ARM, the viewer (like the game's data) takes it as signed, as on x86. -Wl,-q keeps the relocations
# vita-elf-create needs.
CFLAGS="-std=gnu99 -O2 -ffunction-sections -fsigned-char -Wall -Wno-format-truncation -mcpu=cortex-a9 -mfpu=neon
        -D_FILE_OFFSET_BITS=64 -Isrc/gles -Isrc -I$VITASDK/arm-vita-eabi/include/SDL2"
LIBS="-lSDL2 -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub -lSceKernelDmacMgr_stub
      -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceTouch_stub -lSceHid_stub -lSceMotion_stub -lSceAudio_stub
      -lSceAudioIn_stub -lSceAppMgr_stub -lSceAppUtil_stub -lSceCommonDialog_stub -lSceIme_stub -lScePower_stub
      -lSceSysmodule_stub -lSceIofilemgr_stub -lstdc++ -lpthread -lm"
mkdir -p build/vita
echo "Building $OUT.vpk (RampageRE $VERSION, APP_VER $APP_VER) ..."
# shellcheck disable=SC2086
arm-vita-eabi-gcc $CFLAGS -Wl,-q -Wl,-Map,build/vita/$OUT.map -o build/vita/$OUT.elf $SRC $LIBS
vita-elf-create build/vita/$OUT.elf build/vita/$OUT.velf
vita-make-fself -s -c build/vita/$OUT.velf build/vita/eboot.bin
# ATTRIBUTE2=12: the larger memory budget (plat_sdl.c's heap)
vita-mksfoex -s TITLE_ID=$TITLE_ID -s APP_VER="$APP_VER" -d ATTRIBUTE2=12 "RampageRE" build/vita/param.sfo
vita-pack-vpk -s build/vita/param.sfo -b build/vita/eboot.bin \
    -a res/vita/icon0.png=sce_sys/icon0.png \
    -a res/vita/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
    -a res/vita/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
    -a res/vita/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
    "$OUT.vpk"
echo "Done: $OUT.vpk"
