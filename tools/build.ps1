# Builds the x86 proxy DLL with the bundled VS Build Tools CMake.
#
#   pwsh -File tools\build.ps1
#   pwsh -File tools\build.ps1 -Config Debug
#   pwsh -File tools\build.ps1 -Install    (copy into the game dir + chain DXVK)

param(
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$Root    = Split-Path $PSScriptRoot -Parent
$Build   = Join-Path $Root 'build'
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2"

# --- uninstall: put the game back the way we found it ---------------------
# Run this before touching multiplayer. PunkBuster ships with this game.
if ($Uninstall) {
    $p = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
    if ($p) { throw "BFBC2Game.exe is running (pid $($p.Id)). Close it first." }

    $target     = Join-Path $GameDir 'd3d9.dll'
    $dxvkName   = Join-Path $GameDir 'dxvk_d3d9.dll'
    $oursMarker = Join-Path $GameDir 'd3d9.dll.bfbc2vr-installed'
    $dxvkMarker = Join-Path $GameDir 'd3d9.dll.dxvk-installed'

    if (Test-Path $oursMarker) {
        Remove-Item $target -Force -ErrorAction SilentlyContinue
        Remove-Item $oursMarker -Force
        Write-Host "Removed bfbc2vr proxy." -ForegroundColor Green
    } elseif (Test-Path $target) {
        Write-Host "d3d9.dll present but not ours - left alone." -ForegroundColor Yellow
    }

    # Put DXVK back where it was, so -Disable in dxvk.ps1 still understands it.
    if (Test-Path $dxvkName) {
        Move-Item $dxvkName $target -Force
        "DXVK restored by tools/build.ps1 -Uninstall" | Set-Content $dxvkMarker
        Write-Host "Restored DXVK to d3d9.dll. Run tools\dxvk.ps1 -Disable for fully stock." -ForegroundColor Cyan
    }

    Write-Host "`nDone. Verify with: pwsh -File tools\dxvk.ps1 -Status"
    exit 0
}

$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) {
    $found = Get-Command cmake -ErrorAction SilentlyContinue
    if ($found) { $cmake = $found.Source } else { throw "cmake not found" }
}

if ($Clean -and (Test-Path $Build)) {
    Remove-Item $Build -Recurse -Force
    Write-Host "Cleaned build directory."
}

# -A Win32 is mandatory. BFBC2Game.exe is 32-bit; an x64 DLL will not load.
Write-Host "=== Configure (x86) ===" -ForegroundColor Cyan
& $cmake -B $Build -S $Root -A Win32
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "`n=== Build ($Config) ===" -ForegroundColor Cyan
& $cmake --build $Build --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$dll = Join-Path $Build "$Config\d3d9.dll"
if (-not (Test-Path $dll)) { throw "expected output missing: $dll" }

# Confirm we actually produced a 32-bit binary rather than trusting the generator.
$bytes = [System.IO.File]::ReadAllBytes($dll)
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
$arch = switch ($machine) { 0x14c { 'x86' } 0x8664 { 'x64' } default { "0x{0:X}" -f $machine } }

Write-Host "`nBuilt: $dll"
Write-Host "Arch : $arch" -ForegroundColor $(if ($arch -eq 'x86') { 'Green' } else { 'Red' })
if ($arch -ne 'x86') { throw "wrong architecture - BFBC2Game.exe is 32-bit" }

if (-not $Install) {
    Write-Host "`nRe-run with -Install to deploy into the game directory." -ForegroundColor Cyan
    exit 0
}

# --- install -------------------------------------------------------------
$p = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
if ($p) { throw "BFBC2Game.exe is running (pid $($p.Id)). Close it before installing." }

$target     = Join-Path $GameDir 'd3d9.dll'
$dxvkName   = Join-Path $GameDir 'dxvk_d3d9.dll'
$oursMarker = Join-Path $GameDir 'd3d9.dll.bfbc2vr-installed'
$dxvkMarker = Join-Path $GameDir 'd3d9.dll.dxvk-installed'

# Three cases for an existing d3d9.dll, and they must stay distinguishable
# across repeat installs - otherwise a rebuild refuses to replace its own
# previous output, and you silently keep testing a stale DLL.
if (Test-Path $target) {
    if (Test-Path $oursMarker) {
        Write-Host "Replacing our previously installed proxy." -ForegroundColor DarkGray
    }
    elseif (Test-Path $dxvkMarker) {
        Move-Item $target $dxvkName -Force
        Remove-Item $dxvkMarker -Force
        Write-Host "Moved DXVK aside -> dxvk_d3d9.dll (we now chain in front of it)" -ForegroundColor Yellow
    }
    else {
        throw "d3d9.dll present but installed by neither tools/dxvk.ps1 nor this script - refusing to overwrite. Inspect it first."
    }
}

Copy-Item $dll $target -Force
"bfbc2vr proxy installed by tools/build.ps1 ($Config)" | Set-Content $oursMarker
Write-Host "Installed proxy -> $target" -ForegroundColor Green

if (Test-Path $dxvkName) {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> dxvk_d3d9.dll -> Vulkan" -ForegroundColor Green
} else {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> system D3D9" -ForegroundColor Yellow
    Write-Host "  (no DXVK found; run tools\dxvk.ps1 -Enable first to chain it)"
}
Write-Host "`nSINGLEPLAYER ONLY - PunkBuster ships with this game." -ForegroundColor Yellow
Write-Host "Log will be written to $GameDir\bfbc2vr.log"
