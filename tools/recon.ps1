# Re-runnable environment probe for the BFBC2 VR project.
# Prints target binary facts, graphics/input surface, and config state.
# Usage:  pwsh -File tools\recon.ps1

$ErrorActionPreference = 'Continue'

function Find-GameDir {
    $roots = @(
        "C:\Program Files (x86)\Steam\steamapps\common",
        "C:\Program Files\Steam\steamapps\common",
        "C:\Games\steamapps\common",
        "D:\SteamLibrary\steamapps\common",
        "E:\SteamLibrary\steamapps\common",
        "C:\Program Files (x86)\Origin Games",
        "C:\Program Files\Electronic Arts"
    )
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $hit = Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -match 'Bad Company|BFBC' } |
                   Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

$game = Find-GameDir
if (-not $game) { Write-Host "Game directory not found." -ForegroundColor Red; exit 1 }

Write-Host "=== Install ===" -ForegroundColor Cyan
Write-Host $game

$exe = Join-Path $game "BFBC2Game.exe"
if (-not (Test-Path $exe)) { Write-Host "BFBC2Game.exe missing." -ForegroundColor Red; exit 1 }

Write-Host "`n=== Target binary ===" -ForegroundColor Cyan
$bytes = [System.IO.File]::ReadAllBytes($exe)
$peOff  = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
$arch = switch ($machine) { 0x14c { "x86 (32-bit)" } 0x8664 { "x64" } default { "0x{0:X}" -f $machine } }
$stamp = [BitConverter]::ToUInt32($bytes, $peOff + 8)

[PSCustomObject]@{
    Architecture = $arch
    Sections     = [BitConverter]::ToUInt16($bytes, $peOff + 6)
    PETimestamp  = [datetimeoffset]::FromUnixTimeSeconds($stamp).UtcDateTime
    FileVersion  = (Get-Item $exe).VersionInfo.FileVersion
    SizeMB       = [math]::Round((Get-Item $exe).Length / 1MB, 2)
    SHA256       = (Get-FileHash $exe -Algorithm SHA256).Hash
} | Format-List

Write-Host "=== Graphics / input DLL references ===" -ForegroundColor Cyan
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
[regex]::Matches($ascii, '[A-Za-z0-9_]{3,24}\.dll') |
    ForEach-Object { $_.Value.ToLower() } |
    Sort-Object -Unique |
    Where-Object { $_ -match 'd3d|dxgi|xinput|dinput|opengl|nvapi|binkw|^pb' }

Write-Host "`n=== Injected DLLs present in game dir ===" -ForegroundColor Cyan
$proxies = @('d3d9.dll','d3d11.dll','dxgi.dll','dinput8.dll','xinput1_3.dll','winmm.dll','version.dll')
$found = Get-ChildItem $game -File | Where-Object { $proxies -contains $_.Name.ToLower() }
if ($found) { $found | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize }
else { Write-Host "(none — stock install)" }

Write-Host "=== Config ===" -ForegroundColor Cyan
$ini = "$env:USERPROFILE\Documents\BFBC2\settings.ini"
if (Test-Path $ini) {
    Write-Host $ini
    Get-Content $ini | Where-Object { $_ -match 'DxVersion|Fov|Resolution|Fullscreen|VSync' }
} else {
    Write-Host "settings.ini not present - launch the game once to generate it." -ForegroundColor Yellow
}

Write-Host "`n=== PunkBuster ===" -ForegroundColor Cyan
$pb = Join-Path $game "pb"
if (Test-Path $pb) {
    Write-Host "PRESENT at $pb - SINGLEPLAYER ONLY. Never inject into an online session." -ForegroundColor Yellow
} else {
    Write-Host "(no pb directory)"
}
