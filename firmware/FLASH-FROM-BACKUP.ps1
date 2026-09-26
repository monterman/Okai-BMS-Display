# FLASH-FROM-BACKUP.ps1 — reflash a saved Okai BMS Display binary with NO compiling.
#   .\firmware\FLASH-FROM-BACKUP.ps1 -Port COM4
#   .\firmware\FLASH-FROM-BACKUP.ps1 -Port COM4 -Set "OKAI-v0.3.0-2026-09-25-cb162b0-FLASHED"
# Omit -Set and it lists what is saved, newest first, and uses the newest. ~30 s.
#
# YOUR LOGS AND SETTINGS SURVIVE: nothing is written at or above 0x610000, where the
# ffat partition holding the CSV logs lives. Only bootloader, partition table and app.
#
# This board is an ESP32-S3 (LILYGO T-Display-S3). Do NOT point it at a BREmote set —
# those are ESP32-C3 with a different partition layout.
param(
  [Parameter(Mandatory=$true)][string]$Port,
  [string]$Set
)
$ErrorActionPreference = "Stop"
$root    = $PSScriptRoot
$esptool = (Get-ChildItem "C:\Users\Andress Montero\AppData\Local\Arduino15\packages\esp32\tools\esptool_py" -Recurse -Filter esptool.exe | Select-Object -First 1).FullName

$sets = Get-ChildItem $root -Directory | Sort-Object Name -Descending
if (-not $sets) { throw "No saved sets in $root" }
Write-Host "Saved Okai sets:"; $sets | ForEach-Object { Write-Host "  $($_.Name)" }
if (-not $Set) { $Set = $sets[0].Name; Write-Host "`nNo -Set given; using the newest: $Set" }
$dir = Join-Path $root $Set
if (-not (Test-Path $dir)) { throw "Set not found: $dir" }

# Safety: confirm the board on this port really is the LILYGO before writing to it.
Write-Host "`nVerifying the board on $Port ..."
$mac = (& $esptool --port $Port read-mac 2>&1 | Select-String "MAC:" | Select-Object -First 1)
Write-Host "  $mac"
if ("$mac" -notmatch "a0:f2:62:e1:e9:ec") {
  Write-Host "  WARNING: that is not the known Okai MAC (a0:f2:62:e1:e9:ec)." -ForegroundColor Yellow
  $ans = Read-Host "  Continue anyway? (type YES)"
  if ($ans -ne "YES") { throw "Aborted — wrong board." }
}

$app  = Get-ChildItem $dir -Filter "*.ino.bin" | Select-Object -First 1
$boot = Get-ChildItem $dir -Filter "*.bootloader.bin" | Select-Object -First 1
$part = Get-ChildItem $dir -Filter "*.partitions.bin" | Select-Object -First 1
$ba   = Join-Path $dir "boot_app0.bin"
if (-not $app) { throw "No application .ino.bin in $dir" }

Write-Host "`nFlashing Okai on $Port from $Set ..."
$args = @("--chip","esp32s3","--port",$Port,"--baud","921600","write-flash","-z")
if ($boot) { $args += @("0x0",   $boot.FullName) }
if ($part) { $args += @("0x8000",$part.FullName) }
if (Test-Path $ba) { $args += @("0xe000", $ba) }
$args += @("0x10000", $app.FullName)
& $esptool @args
if ($LASTEXITCODE -ne 0) { Write-Host "`nFLASH FAILED (exit $LASTEXITCODE). Try a lower baud: edit 921600 -> 460800." -ForegroundColor Red }
else { Write-Host "`nDone. Power-cycle, then re-sync the clock from the WiFi dashboard." -ForegroundColor Green }
