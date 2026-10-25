@echo off
rem make_hd_textures.bat [anime] [disc image or RAMPAGE.VOL] - an AI-upscaled texture pack for RampageRE, made on your
rem own PC (docs\TEXTURES.md 5; after WoodyRE's).
rem   make_hd_textures.bat          4x with realesr-animevideov3 (stays close to the original art)
rem   make_hd_textures.bat anime    4x with realesrgan-x4plus-anime (crisper edges, flatter noisy surfaces; slower)
rem Without a disc image the data RampageRE finds by itself is taken (docs\NATIVE.md "Game data").
rem Steps: RampageRE.exe --dumptex all writes every texture of the game to mods\dump; Real-ESRGAN ncnn-vulkan (any Vulkan
rem GPU; downloaded once into tools\realesrgan and checked against its SHA-256) upscales them; texup (built with Zig, like
rem build.bat) keeps repeating textures seamless and see-through edges clean. The result goes to mods\textures\hd, where
rem the game picks it up at the next start. Running it again only does textures that are not in the pack yet: delete
rem single PNGs you do not like (the game then uses the original), or the whole mods\textures\hd folder to start over.
rem The pack is made from the game's own textures: keep it for yourself, do not share it.
setlocal
cd /d "%~dp0"

set "MODEL=realesr-animevideov3-x4"
set "DATA="
for %%a in (%*) do if /i "%%~a"=="anime" (set "MODEL=realesrgan-x4plus-anime") else (set "DATA=%%~fa")
set "ESR_VER=v0.2.5.0"
set "ESR_NAME=realesrgan-ncnn-vulkan-20220424-windows"
set "ESR_SHA=abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d"
set "ESR=%CD%\tools\realesrgan"
set "EXE=%CD%\RampageRE.exe"

if not exist "%EXE%" (
    echo RampageRE.exe is not here yet: run build.bat first, or put this file next to RampageRE.exe.
    goto :fail
)
call "%~dp0build.bat" texup >nul || goto :fail
set "TEXUP=%CD%\build\texup.exe"

if not exist "%ESR%\realesrgan-ncnn-vulkan.exe" (
    echo Real-ESRGAN ncnn-vulkan ^(the upscaler, about 45 MB^) is not here yet: downloading it from github.com/xinntao/Real-ESRGAN ...
    if not exist "%ESR%" mkdir "%ESR%"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol='Tls12'; Invoke-WebRequest -Uri 'https://github.com/xinntao/Real-ESRGAN/releases/download/%ESR_VER%/%ESR_NAME%.zip' -OutFile '%ESR%\esr.zip'; if (([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes('%ESR%\esr.zip'))) -replace '-','') -ne '%ESR_SHA%') { Remove-Item '%ESR%\esr.zip'; throw 'SHA-256 of the download does not match' }" || goto :fail
    tar -xf "%ESR%\esr.zip" -C "%ESR%" || goto :fail
    del "%ESR%\esr.zip"
)

rem the game's home, where it looks for mods\: this folder, or %LOCALAPPDATA%\RampageRE when this one is read-only (as
rem RampageRE.exe started without arguments decides; the standalone exe always uses %LOCALAPPDATA%\RampageRE)
set "HOME_DIR=%CD%"
copy /y nul "%CD%\mods_write_test.tmp" >nul 2>nul && (del "%CD%\mods_write_test.tmp") || set "HOME_DIR=%LOCALAPPDATA%\RampageRE"
if not exist "%HOME_DIR%" mkdir "%HOME_DIR%"
pushd "%HOME_DIR%"
echo Collecting every texture of the game ...
if defined DATA (start "" /wait "%EXE%" "%DATA%" --dumptex all) else (start "" /wait "%EXE%" --dumptex all)
if not exist "mods\dump" (
    echo The game wrote no textures: does RampageRE.exe start normally? See %HOME_DIR%\rampage.log.
    popd & goto :fail
)
"%TEXUP%" pre mods\dump mods\work mods\textures\hd
if errorlevel 3 if not errorlevel 4 (
    echo mods\textures\hd already has every texture. Delete it to make the pack again.
    popd & exit /b 0
)
if errorlevel 1 (popd & goto :fail)
if not exist mods\work\out mkdir mods\work\out
echo Upscaling with %MODEL% ^(this takes a while; the GPU does the work^) ...
"%ESR%\realesrgan-ncnn-vulkan.exe" -i mods\work\in -o mods\work\out -n %MODEL% -s 4 -f png -m "%ESR%\models" >nul 2>nul
"%TEXUP%" post mods\dump mods\work mods\textures\hd || (popd & goto :fail)
rmdir /s /q mods\work
popd
echo.
echo Done: %HOME_DIR%\mods\textures\hd. Start the game to see it; delete that folder to go back to the original textures.
echo For the standalone exe, copy the hd folder into %LOCALAPPDATA%\RampageRE\mods\textures\.
exit /b 0

:fail
echo.
echo Making the texture pack FAILED.
exit /b 1
