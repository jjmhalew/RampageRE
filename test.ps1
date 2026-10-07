<#
Starts the built game for a few seconds and keeps its output in build\test.out / build\test.err.

  .\test.ps1            run 45 seconds
  .\test.ps1 -Seconds 120
#>
param([int]$Seconds = 45)
$root = $PSScriptRoot
$exe = Join-Path $root "build\cmake\ps2xRuntime\Release\ps2EntryRunner.exe"
if (-not (Test-Path $exe)) { throw "Build first: .\build.ps1" }
Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
$out = Join-Path $root "build\test.out"
$err = Join-Path $root "build\test.err"
$env:PS2X_AUTOPRESS = "1"
$p = Start-Process -FilePath $exe -ArgumentList "RAMPAGE.ELF" -WorkingDirectory (Join-Path $root "build\disc") `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if ($p.WaitForExit($Seconds * 1000)) { "exited with code $($p.ExitCode)" } else { $p | Stop-Process -Force; "still running after $Seconds s (stopped)" }
Get-Content $err | ForEach-Object { if ($_.Length -gt 300) { $_.Substring(0, 300) + "..." } else { $_ } }
