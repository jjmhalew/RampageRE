# The native part of the Android build: SDL2 (fetched, the release whose Java glue is in src/main/java/org/libsdl) and the
# viewer of ../../src as libmain.so, on OpenGL ES 3.0 / 2.0 through ../../src/gles (GL/gl.h there replaces the desktop
# header). The SRC list is build_switch.sh's (build.sh's with src/gles/gles2.c; keep them in step) with
# src/android.c and src/touch.c.
cmake_minimum_required(VERSION 3.18)
project(RampageRE C)

include(FetchContent)
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
add_library(main SHARED
    ${S}/main.c ${S}/shot.c ${S}/render_gl.c ${S}/city.c ${S}/vol.c ${S}/bundle.c ${S}/tex.c ${S}/mesh.c ${S}/prb.c
    ${S}/tree.c ${S}/anim.c ${S}/camera.c ${S}/moves.c ${S}/building.c ${S}/climb.c ${S}/ik.c ${S}/people.c ${S}/fx.c
    ${S}/cloth.c ${S}/sound.c ${S}/army.c ${S}/hud.c ${S}/vehicle.c ${S}/water.c ${S}/edible.c ${S}/game.c ${S}/boss.c
    ${S}/look.c ${S}/shadow.c ${S}/director.c ${S}/footstep.c ${S}/challenge.c ${S}/reveal.c ${S}/rail.c ${S}/parade.c
    ${S}/streetobj.c ${S}/profile.c ${S}/collide.c ${S}/shell.c ${S}/movie.c ${S}/wrand.c ${S}/texpack.c ${S}/iso.c
    ${S}/plat_sdl.c ${S}/pad_sdl.c ${S}/settings.c ${S}/postfx.c ${S}/gtao.c ${S}/autoplay.c
    ${S}/gles/gles2.c ${S}/android.c ${S}/touch.c)
target_include_directories(main PRIVATE ${S}/gles ${S})
# char is unsigned on ARM; the viewer is written for x86, where it is signed (as GLES=1 build.sh)
target_compile_options(main PRIVATE -std=gnu99 -O2 -fsigned-char -Wall -Wno-format-truncation)
target_compile_definitions(main PRIVATE _FILE_OFFSET_BITS=64)
set_source_files_properties(${S}/main.c PROPERTIES COMPILE_DEFINITIONS main=SDL_main)   # SDLActivity calls SDL_main
# GLESv3: the blit and multisample calls of postfx.c / gtao.c (src/gles/gles2.c)
target_link_libraries(main PRIVATE SDL2::SDL2 GLESv2 GLESv3 log android m)
