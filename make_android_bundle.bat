@echo off
rem make_android_bundle.bat [RampageRE.apk] [disc image or RAMPAGE.VOL] - one APK with everything inside (RampageRE-bundle.apk):
rem the Android app + YOUR game data (RAMPAGE.VOL, the game's ELF and SYSTEM.CNF from your disc, about 1.35 GB; not the
rem whole disc image). The app reads them in place from its own APK: it neither asks for the disc nor copies anything.
rem Drag the APK and your disc image (.iso) or its RAMPAGE.VOL onto this file, or put them next to it (the newest
rem RampageRE-*.apk is taken, else the one android\ built; any *.iso). Without a disc image the data RampageRE finds by
rem itself is taken (docs\NATIVE.md "Game data": build\disc, %LOCALAPPDATA%\RampageRE, ...).
rem Because it contains the game's data it is for your own devices only: never share or upload it.
rem It is signed with a key of your own (made once, kept in %LOCALAPPDATA%\RampageRE\android-bundle.key): later bundles
rem install as updates of it, but not over a RampageRE from the Releases page - uninstall that first (its saves and copied
rem game files go with it). After WoodyRE's.
setlocal
cd /d "%~dp0"
set "APK="
set "DATA="
for %%a in (%*) do if /i "%%~xa"==".apk" (set "APK=%%~fa") else (set "DATA=%%~fa")
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
build\apkbundle.exe "%APK%" "%DATA%" RampageRE-bundle.apk "%LOCALAPPDATA%\RampageRE\android-bundle.key" "RampageRE bundle" || goto :fail
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
