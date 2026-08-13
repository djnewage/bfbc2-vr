# Builds the x86 proxy DLL with the bundled VS Build Tools CMake.
#
#   pwsh -File tools\build.ps1
#   pwsh -File tools\build.ps1 -Config Debug
#   pwsh -File tools\build.ps1 -Install    (copy into the game dir + chain DXVK)

param(
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [switch]$Install,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$Root    = Split-Path $PSScriptRoot -Parent
$Build   = Join-Path $Root 'build'
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2"

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

$target   = Join-Path $GameDir 'd3d9.dll'
$dxvkName = Join-Path $GameDir 'dxvk_d3d9.dll'

# If DXVK currently occupies d3d9.dll, rename it so we can chain in front of it.
if ((Test-Path $target) -and (Test-Path (Join-Path $GameDir 'd3d9.dll.dxvk-installed'))) {
    Move-Item $target $dxvkName -Force
    Remove-Item (Join-Path $GameDir 'd3d9.dll.dxvk-installed') -Force
    Write-Host "Moved DXVK aside -> dxvk_d3d9.dll (we now chain in front of it)" -ForegroundColor Yellow
}
elseif (Test-Path $target) {
    throw "d3d9.dll already present and not installed by tools/dxvk.ps1 - refusing to overwrite. Inspect it first."
}

Copy-Item $dll $target -Force
Write-Host "Installed proxy -> $target" -ForegroundColor Green

if (Test-Path $dxvkName) {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> dxvk_d3d9.dll -> Vulkan" -ForegroundColor Green
} else {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> system D3D9" -ForegroundColor Yellow
    Write-Host "  (no DXVK found; run tools\dxvk.ps1 -Enable first to chain it)"
}
Write-Host "`nSINGLEPLAYER ONLY - PunkBuster ships with this game." -ForegroundColor Yellow
Write-Host "Log will be written to $GameDir\bfbc2vr.log"
