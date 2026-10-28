@echo off
rem build.bat - builds RampageRE, the native port (src\), on Windows 10/11 with Zig; nothing to install first. After WoodyRE's.
rem   build.bat                     RampageRE.exe, the player build: no console, starts in the front end, asks for your
rem                                 disc image at the first start, logs to rampage.log
rem   build.bat dev                 RampageRE-dev.exe, the developer build: a console that prints what each block holds (the
rem                                 build the tests and docs\NATIVE.md use; build_viewer.bat builds this one)
rem   build.bat standalone [DATA] [WII]  RampageRE-standalone.exe: RampageRE.exe + YOUR game data in one exe, for your
rem                                 own use only (make_standalone.bat); DATA = your disc image or RAMPAGE.VOL, else the
rem                                 data the viewer finds (docs\NATIVE.md "Game data"); WII = your extracted Wii disc,
rem                                 else rampage.cfg's wii_data= or a wii folder here, if any (docs\WII_PLAN.md)
rem   build.bat apkbundle           only build\apkbundle.exe, the tool of make_android_bundle.bat
rem   build.bat tools               only the helpers of tools\native\ in build\: bundle_stub.exe and pack.exe (standalone),
rem                                 apkbundle.exe (the CI builds them)
rem Linux: build.sh (SDL2), the same SRC list plus src\plat_sdl.c, and src\pad_sdl.c for src\pad.c; the Switch:
rem build_switch.sh, that list with src\gles\gles2.c; Android: android\app\CMakeLists.txt, that list with src\android.c
rem and src\touch.c; keep the four in step.
rem The C compiler is Zig (ziglang.org): tools\zig, a zig on PATH or "pip install ziglang" is used when there,
rem otherwise the official Windows build is downloaded once into tools\zig and checked against its SHA-256.
setlocal
cd /d "%~dp0"

set "ZIG_VER=0.16.0"
set "ZIG_SHA=68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"
set "ZIG_NAME=zig-x86_64-windows-%ZIG_VER%"
set "SRC=src\main.c src\shot.c src\render_gl.c src\city.c src\vol.c src\bundle.c src\tex.c src\mesh.c src\prb.c src\tree.c src\anim.c src\camera.c src\moves.c src\building.c src\climb.c src\ik.c src\people.c src\fx.c src\cloth.c src\sound.c src\army.c src\hud.c src\vehicle.c src\water.c src\edible.c src\game.c src\boss.c src\look.c src\shadow.c src\director.c src\footstep.c src\challenge.c src\reveal.c src\rail.c src\parade.c src\streetobj.c src\profile.c src\collide.c src\shell.c src\movie.c src\wrand.c src\texpack.c src\iso.c src\pad.c src\settings.c src\postfx.c src\gtao.c src\autoplay.c"
set "LIBS=-lopengl32 -lgdi32 -luser32 -lhid -lsetupapi -lcomdlg32 -lole32 -ldbghelp"
rem zig cc compiles for the CPU of the build machine unless told otherwise: an exe built on a new PC or on the CI runner
rem would then use AVX2 / AVX-512 and die with "illegal instruction" on older CPUs. Plain x86-64 runs everywhere.
set "CPU=-target x86_64-windows-gnu -mcpu=baseline"
rem the helpers of tools\native\ read the disc image with src\iso.c (RAMPAGE_TOOL: without its file dialog)
set "TOOL=-std=c99 -O2 -Wall %CPU% -DRAMPAGE_TOOL -Isrc"

call :find_zig || goto :fail

if /i "%~1"=="apkbundle" (
    call :apkbundle || goto :fail
    exit /b 0
)
if /i "%~1"=="tools" (
    call :standalone_tools || goto :fail
    call :apkbundle || goto :fail
    exit /b 0
)
if /i "%~1"=="dev" (
    echo Building RampageRE-dev.exe ^(developer build^)...
    %ZIG% cc -std=c99 -O2 -Wall %CPU% -o RampageRE-dev.exe %SRC% res\rampagere.rc %LIBS% || goto :fail
    echo Done: RampageRE-dev.exe
    exit /b 0
)

echo Building RampageRE.exe...
%ZIG% cc -std=c99 -O2 -Wall %CPU% -DRAMPAGE_GUI -Wl,--subsystem,windows -o RampageRE.exe %SRC% res\rampagere.rc %LIBS% || goto :fail
echo Done: RampageRE.exe
if /i not "%~1"=="standalone" exit /b 0

rem ---- standalone: the launcher stub + RampageRE.exe + RAMPAGE.VOL, the ELF and SYSTEM.CNF of your disc ----
call :standalone_tools || goto :fail
set "DATA=%~2"
if not defined DATA set "DATA=auto"
set "WII=%~3"
if not defined WII set "WII=auto"
echo Packing the game data ^(%DATA%; the Wii's: %WII%^) ...
build\pack.exe build\bundle_stub.exe RampageRE.exe "%DATA%" RampageRE-standalone.exe "%WII%" || goto :fail
echo.
echo Done: RampageRE-standalone.exe. It contains the game's data - keep it for yourself, never share or upload it.
exit /b 0

:standalone_tools
if not exist build mkdir build
echo Building build\bundle_stub.exe and build\pack.exe...
%ZIG% cc -std=c99 -O2 -Wall %CPU% -Wl,--subsystem,windows -o build\bundle_stub.exe tools\native\bundle.c res\rampagere.rc -luser32 -lgdi32 || exit /b 1
%ZIG% cc %TOOL% -o build\pack.exe tools\native\pack.c src\iso.c -lshell32 || exit /b 1
exit /b 0

:apkbundle
if not exist build mkdir build
echo Building build\apkbundle.exe...
%ZIG% cc %TOOL% -o build\apkbundle.exe tools\native\apkbundle.c src\iso.c -lbcrypt -lshell32 || exit /b 1
echo Done: build\apkbundle.exe
exit /b 0

:find_zig
if exist "tools\zig\%ZIG_NAME%\zig.exe" (set ZIG="%CD%\tools\zig\%ZIG_NAME%\zig.exe" & exit /b 0)
where zig >nul 2>nul && (set "ZIG=zig" & exit /b 0)
python -m ziglang version >nul 2>nul && (set "ZIG=python -m ziglang" & exit /b 0)
echo Zig %ZIG_VER% (the C compiler, about 95 MB) is not here yet: downloading it from ziglang.org into tools\zig ...
if not exist tools\zig mkdir tools\zig
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri 'https://ziglang.org/download/%ZIG_VER%/%ZIG_NAME%.zip' -OutFile 'tools\zig\zig.zip'; if ((Get-FileHash 'tools\zig\zig.zip' -Algorithm SHA256).Hash -ne '%ZIG_SHA%') { Remove-Item 'tools\zig\zig.zip'; throw 'SHA-256 of the download does not match' }" || exit /b 1
tar -xf tools\zig\zig.zip -C tools\zig || exit /b 1
del tools\zig\zig.zip
if not exist "tools\zig\%ZIG_NAME%\zig.exe" (echo The Zig archive did not contain %ZIG_NAME%\zig.exe & exit /b 1)
set ZIG="%CD%\tools\zig\%ZIG_NAME%\zig.exe"
exit /b 0

:fail
echo.
echo Build FAILED.
exit /b 1
