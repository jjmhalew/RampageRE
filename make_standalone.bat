@echo off
rem make_standalone.bat [disc image or RAMPAGE.VOL] - one exe with everything inside (RampageRE-standalone.exe): RampageRE.exe
rem + YOUR game data (RAMPAGE.VOL, the game's ELF and SYSTEM.CNF from your disc, about 1.35 GB). It unpacks itself to
rem %LOCALAPPDATA%\RampageRE at its first start (again only when a newer bundle changes something) and starts the game
rem from there. Drag your disc image (.iso) or its RAMPAGE.VOL onto this file; without one the data RampageRE finds by
rem itself is taken (docs\NATIVE.md "Game data": build\disc, the current folder, %LOCALAPPDATA%\RampageRE, the disc
rem image chosen at an earlier start). Because it contains the game's data it is for your own use only: never share or
rem upload it. After WoodyRE's.
call "%~dp0build.bat" standalone "%~1"
if errorlevel 1 pause
