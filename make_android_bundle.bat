@echo off
rem make_android_bundle.bat [RampageRE.apk] [disc image or RAMPAGE.VOL] [Wii folder] - one APK with everything inside
rem (RampageRE-bundle.apk): the Android app + YOUR game data (RAMPAGE.VOL, the game's ELF and SYSTEM.CNF from your disc,
rem about 1.35 GB; not the whole disc image). The app reads them in place from its own APK: it neither asks for the disc
rem nor copies anything. Drag the APK and your disc image (.iso) or its RAMPAGE.VOL onto this file, or put them next to it
rem (the newest RampageRE-*.apk is taken, else the one android\ built; any *.iso). Without a disc image the data RampageRE
rem finds by itself is taken (docs\NATIVE.md "Game data": build\disc, %LOCALAPPDATA%\RampageRE, ...).
rem Optional, the Wii's monsters and Dallas (docs\WII_PLAN.md): drag your extracted Wii disc's folder (Dolphin's Extract
rem Entire Disc: the folder with DATA, or its DATA\files) onto it too; without one, rampage.cfg's wii_data= or a wii
rem folder here is taken when there is one. Its Game, Shell and Audio go in (about 320 MB more).
rem Because it contains the game's data it is for your own devices only: never share or upload it.
rem It is signed with a key of your own (made once, kept in %LOCALAPPDATA%\RampageRE\android-bundle.key): later bundles
rem install as updates of it, but not over a RampageRE from the Releases page - uninstall that first (its saves and copied
rem game files go with it). After WoodyRE's.
setlocal
cd /d "%~dp0"
set "APK="
set "DATA="
set "WII=auto"
for %%a in (%*) do (
    if /i "%%~xa"==".apk" (set "APK=%%~fa") else if exist "%%~fa\*" (set "WII=%%~fa") else (set "DATA=%%~fa")
)
if not defined APK for /f "delims=" %%f in ('dir /b /o:d RampageRE-*.apk 2^>nul ^| findstr /v /i /l /x "RampageRE-bundle.apk"') do set "APK=%CD%\%%f"
if not defined APK if exist "android\app\build\outputs\apk\release\app-release.apk" set "APK=%CD%\android\app\build\outputs\apk\release\app-release.apk"
if not defined APK if exist "android\app\build\outputs\apk\debug\app-debug.apk" set "APK=%CD%\android\app\build\outputs\apk\debug\app-debug.apk"
if not defined DATA for %%f in (*.iso) do set "DATA=%%~ff"
if not defined DATA set "DATA=auto"
if not defined APK (
    echo No APK: drag RampageRE-^<version^>.apk from the Releases page onto this file, or put it next to it.
    goto :fail
)
call "%~dp0build.bat" apkbundle || goto :fail
echo APK: %APK%
echo Game data: %DATA%
echo Wii data: %WII%
build\apkbundle.exe "%APK%" "%DATA%" RampageRE-bundle.apk "%LOCALAPPDATA%\RampageRE\android-bundle.key" "RampageRE bundle" "%WII%" || goto :fail
echo.
echo Done: RampageRE-bundle.apk. Install it on your phone or tablet (a USB cable, or adb install RampageRE-bundle.apk).
echo It contains the game's data - keep it for yourself, never share or upload it.
pause
exit /b 0

:fail
echo.
echo No bundle made.
pause
exit /b 1
