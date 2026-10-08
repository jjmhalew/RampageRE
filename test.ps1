<#
Starts the built game for a few seconds and keeps its output in build\test.out / build\test.err.
The runtime writes a status line and build\disc\shot_NNN.png every 600 frames (PS2X_SHOT_EVERY changes that).

  .\test.ps1                         run 45 seconds; Start and Cross are tapped every 6 s
  .\test.ps1 -Seconds 120
  .\test.ps1 -PadInput "12:cross;20.5:down/0.3"   scripted pad input (seconds after start; see ps2_pad.cpp)
  .\test.ps1 -ToGameplay             create a profile and start the campaign (Las Vegas) with George
#>
param([int]$Seconds = 45, [string]$PadInput = "", [switch]$ToGameplay)
$root = $PSScriptRoot
$exe = Join-Path $root "build\cmake\ps2xRuntime\Release\ps2EntryRunner.exe"
if (-not (Test-Path $exe)) { throw "Build first: .\build.ps1" }

if ($ToGameplay) {
    # Timed for the current speed (about 8 game frames per second): autosave notice, the intro movies, main menu
    # (CAMPAIGN), Create New Profile, type "AA", cursor to DONE, then Cross through the menus into the level.
    $c = [Globalization.CultureInfo]::InvariantCulture
    $steps = @("12:cross", "15:cross", "18:cross", "72:cross", "80:cross", "86:cross", "89:cross")
    $t = 92.0
    foreach ($i in 1..3) { $steps += $t.ToString($c) + ":down/0.4"; $t += 1.5 }
    foreach ($i in 1..10) { $steps += $t.ToString($c) + ":right/0.4"; $t += 1.5 }
    $steps += ($t + 1).ToString($c) + ":cross"
    $t += 7
    foreach ($i in 1..14) { $steps += $t.ToString($c) + ":cross"; $t += 5 }
    $PadInput = $steps -join ";"
    if ($Seconds -lt 230) { $Seconds = 230 }
}

Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
$out = Join-Path $root "build\test.out"
$err = Join-Path $root "build\test.err"
$env:PS2X_AUTOPRESS = "1"
$env:PS2X_INPUT = $PadInput   # empty = autopress Start/Cross
$p = Start-Process -FilePath $exe -ArgumentList "RAMPAGE.ELF" -WorkingDirectory (Join-Path $root "build\disc") `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if ($p.WaitForExit($Seconds * 1000)) { "exited with code $($p.ExitCode)" } else { $p | Stop-Process -Force; "still running after $Seconds s (stopped)" }
Get-Content $err | ForEach-Object { if ($_.Length -gt 300) { $_.Substring(0, 300) + "..." } else { $_ } }
