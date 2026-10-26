@echo off
rem make_standalone.bat [disc image or RAMPAGE.VOL] [Wii folder] - one exe with everything inside
rem (RampageRE-standalone.exe): RampageRE.exe + YOUR game data (RAMPAGE.VOL, the game's ELF and SYSTEM.CNF from your disc,
rem about 1.35 GB). It unpacks itself to %LOCALAPPDATA%\RampageRE at its first start (again only when a newer bundle
rem changes something) and starts the game from there. Drag your disc image (.iso) or its RAMPAGE.VOL onto this file;
rem without one the data RampageRE finds by itself is taken (docs\NATIVE.md "Game data": build\disc, the current folder,
rem %LOCALAPPDATA%\RampageRE, the disc image chosen at an earlier start).
rem Optional, the Wii's monsters and Dallas (docs\WII_PLAN.md): drag your extracted Wii disc's folder (Dolphin's Extract
rem Entire Disc: the folder with DATA, or its DATA\files) onto it too; without one, rampage.cfg's wii_data= or a wii
rem folder here is taken when there is one. Its Game, Shell and Audio go in (about 320 MB more) and are unpacked into
rem %LOCALAPPDATA%\RampageRE\wii. Because it contains the game's data it is for your own use only: never share or upload
rem it. After WoodyRE's.
setlocal
set "DATA="
set "WII="
for %%a in (%*) do (
    if exist "%%~fa\*" (set "WII=%%~fa") else (set "DATA=%%~fa")
)
call "%~dp0build.bat" standalone "%DATA%" "%WII%"
if errorlevel 1 pause
