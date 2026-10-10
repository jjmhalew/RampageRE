@echo off
rem build_viewer.bat - builds rampage.exe, RampageRE's native viewer (src\), the WoodyRE way: one C99 compile with Zig.
rem   build_viewer.bat          rampage.exe (console window: it prints what each block holds)
rem Run it from the repository root once build.ps1 has extracted the disc: rampage.exe [RAMPAGE.VOL] [City] [block].
rem The C compiler is Zig (ziglang.org): tools\zig, a zig on PATH or "pip install ziglang" is used when there,
rem otherwise the official Windows build is downloaded once into tools\zig and checked against its SHA-256.
setlocal
cd /d "%~dp0"

set "ZIG_VER=0.16.0"
set "ZIG_SHA=68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"
set "ZIG_NAME=zig-x86_64-windows-%ZIG_VER%"
set "SRC=src\main.c src\shot.c src\render_gl.c src\city.c src\vol.c src\bundle.c src\tex.c src\mesh.c src\prb.c src\tree.c src\anim.c src\camera.c src\moves.c src\building.c src\climb.c src\ik.c src\people.c src\fx.c src\cloth.c src\sound.c src\army.c src\hud.c src\vehicle.c src\water.c src\edible.c src\game.c src\boss.c src\look.c src\shadow.c src\director.c src\footstep.c src\challenge.c src\reveal.c src\rail.c src\parade.c src\streetobj.c src\profile.c src\collide.c src\shell.c"
set "LIBS=-lopengl32 -lgdi32 -luser32"
rem Plain x86-64, so the exe runs on older CPUs too.
set "CPU=-target x86_64-windows-gnu -mcpu=baseline"

call :find_zig || goto :fail
echo Building rampage.exe...
%ZIG% cc -std=c99 -O2 -Wall %CPU% -o rampage.exe %SRC% %LIBS% || goto :fail
echo Done: rampage.exe
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
set ZIG="%CD%\tools\zig\%ZIG_NAME%\zig.exe"
exit /b 0

:fail
echo Build failed.
exit /b 1
