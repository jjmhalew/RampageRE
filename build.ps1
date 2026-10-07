<#
Builds RampageRE from your own Rampage: Total Destruction disc (PS2, Europe, SLES-53800).

  .\build.ps1 -Iso "path\to\Rampage - Total Destruction (Europe).iso"   first time: extracts the disc
  .\build.ps1                                                            later: rebuild only
  .\build.ps1 -Run                                                       rebuild, then start the game

Steps: extract disc -> split ELF -> recompile to C++ (PS2Recomp) -> build ps2EntryRunner.
Needs Python 3, Git and Visual Studio 2022 or later with the C++ workload (its CMake is used).
A full C++ build takes about 30 minutes.
#>
param(
    [string]$Iso,
    [switch]$Run,
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"
$disc = Join-Path $build "disc"
$cmakeDir = Join-Path $build "cmake"
$runtime = Join-Path $root "PS2Recomp\ps2xRuntime"

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

# CMake from Visual Studio, unless one is on PATH
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "CMake not found: install Visual Studio with the C++ workload" }

Step "PS2Recomp submodule"
git -C $root submodule update --init --recursive; Check "git submodule update"
git -C (Join-Path $root "PS2Recomp") apply --reverse --check (Join-Path $root "patches\ps2recomp-runtime.diff") 2>$null
if ($LASTEXITCODE -ne 0) {
    git -C (Join-Path $root "PS2Recomp") apply (Join-Path $root "patches\ps2recomp-runtime.diff"); Check "applying runtime patch"
}

if ($Iso) {
    Step "Extracting disc"
    if (Test-Path $disc) { Remove-Item -Recurse -Force $disc }
    python (Join-Path $root "tools\iso9660.py") $Iso $disc; Check "disc extraction"
}
$elf = Join-Path $disc "SLES_538.00"
if (-not (Test-Path $elf)) { throw "No extracted disc in build\disc. Run: .\build.ps1 -Iso <your Rampage ISO>" }

Step "Splitting ELF"
python (Join-Path $root "tools\split_elf.py") $elf (Join-Path $disc "RAMPAGE.ELF"); Check "split_elf"

Step "Configuring CMake"
& $cmake -S (Join-Path $root "PS2Recomp") -B $cmakeDir -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -Wno-dev | Out-Null; Check "cmake configure"
& $cmake --build $cmakeDir --config $Config --parallel --target ps2_recomp -- /nodeReuse:false /v:minimal | Out-Null; Check "building ps2_recomp"

Step "Recompiling to C++"
$runner = Join-Path $runtime "src\runner"
Get-ChildItem $runner -Filter *.cpp | Where-Object Name -ne "register_functions.cpp" | Remove-Item -Force
Push-Location $root
try {
    & (Join-Path $cmakeDir "ps2xRecomp\$Config\ps2_recomp.exe") "recomp\config.toml" *> (Join-Path $build "recomp.log"); Check "ps2_recomp (see build\recomp.log)"
} finally { Pop-Location }
Move-Item -Force (Join-Path $runner "ps2_recompiled_*.h") (Join-Path $runtime "include")
# MSBuild does not notice regenerated sources inside unity bundles, so drop the old objects
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $cmakeDir "ps2xRuntime\ps2EntryRunner.dir\$Config\unity_*.obj")

Step "Building ps2EntryRunner (about 30 minutes the first time)"
& $cmake $cmakeDir | Out-Null; Check "cmake reconfigure"
& $cmake --build $cmakeDir --config $Config --parallel --target ps2EntryRunner -- /nodeReuse:false /v:minimal *> (Join-Path $build "build.log"); Check "build (see build\build.log)"
$exe = Join-Path $cmakeDir "ps2xRuntime\$Config\ps2EntryRunner.exe"
Write-Host "Built $exe" -ForegroundColor Green

if ($Run) {
    Step "Running"
    Push-Location $disc
    try { & $exe "RAMPAGE.ELF" } finally { Pop-Location }
}
