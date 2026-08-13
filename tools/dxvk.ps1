# Installs / removes DXVK's 32-bit d3d9.dll in the BFBC2 game directory.
#
# DXVK translates D3D9 to Vulkan. It is the backend our mod will sit on top of:
# Vulkan images hand directly to an OpenXR swapchain, whereas raw D3D9 has no
# clean path to a VR runtime. See docs/architecture.md.
#
# This is purely additive - the stock install ships no d3d9.dll, so enabling
# drops one file in and disabling deletes it. Any pre-existing d3d9.dll is
# backed up before being replaced, never silently overwritten.
#
#   pwsh -File tools\dxvk.ps1 -Status
#   pwsh -File tools\dxvk.ps1 -Enable
#   pwsh -File tools\dxvk.ps1 -Disable
#
# !! SINGLEPLAYER ONLY. This game ships PunkBuster. A non-stock d3d9.dll in an
# !! online session is a kick or a ban. Run -Disable before touching multiplayer.

param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Status,
    [string]$Version = '3.0.2'
)

$ErrorActionPreference = 'Stop'

$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2"
if (-not (Test-Path $GameDir)) { Write-Host "Game directory not found: $GameDir" -ForegroundColor Red; exit 1 }

$Target    = Join-Path $GameDir 'd3d9.dll'
$Marker    = Join-Path $GameDir 'd3d9.dll.dxvk-installed'   # records that WE put it there
$StockBak  = Join-Path $GameDir 'd3d9.dll.stock-backup'
$CacheRoot = Join-Path $PSScriptRoot '..\thirdparty' | Resolve-Path -ErrorAction SilentlyContinue
if (-not $CacheRoot) {
    $CacheRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'thirdparty'
    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
}
$DxvkDll = Join-Path $CacheRoot "dxvk-$Version\x32\d3d9.dll"

function Show-Status {
    Write-Host "=== DXVK status ===" -ForegroundColor Cyan
    Write-Host "Game dir : $GameDir"
    if (Test-Path $Target) {
        $f = Get-Item $Target
        $who = if (Test-Path $Marker) { 'DXVK (installed by this script)' } else { 'UNKNOWN - not installed by this script' }
        Write-Host ("d3d9.dll : PRESENT  {0:N0} KB  {1}" -f ($f.Length/1KB), $f.LastWriteTime) -ForegroundColor Yellow
        Write-Host "           $who"
    } else {
        Write-Host "d3d9.dll : absent (stock install)" -ForegroundColor Green
    }
    if (Test-Path $StockBak) { Write-Host "Stock backup preserved at $StockBak" }

    Write-Host "`n=== Vulkan ===" -ForegroundColor Cyan
    $icd = Get-ChildItem 'C:\Windows\System32' -Filter '*vulkan*' -ErrorAction SilentlyContinue |
           Select-Object -ExpandProperty Name
    if ($icd) { $icd } else { Write-Host "no vulkan runtime found in System32" -ForegroundColor Yellow }
    $vi = Get-Command vulkaninfo -ErrorAction SilentlyContinue
    if ($vi) { Write-Host "vulkaninfo available at $($vi.Source)" }
}

function Get-Dxvk {
    if (Test-Path $DxvkDll) { return }
    Write-Host "Fetching DXVK $Version..." -ForegroundColor Cyan
    $tmp = Join-Path $env:TEMP "dxvk-$Version.tar.gz"
    $url = "https://github.com/doitsujin/dxvk/releases/download/v$Version/dxvk-$Version.tar.gz"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
    tar -xzf $tmp -C $CacheRoot
    Remove-Item $tmp -Force
    if (-not (Test-Path $DxvkDll)) { throw "x32/d3d9.dll missing from the DXVK archive" }
}

function Stop-IfRunning {
    $p = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
    if ($p) {
        Write-Host "BFBC2Game.exe is running (pid $($p.Id)). Close it before changing the renderer." -ForegroundColor Red
        exit 1
    }
}

if ($Status -or (-not $Enable -and -not $Disable)) { Show-Status; exit 0 }

if ($Enable) {
    Stop-IfRunning
    Get-Dxvk
    if ((Test-Path $Target) -and -not (Test-Path $Marker)) {
        Copy-Item $Target $StockBak -Force
        Write-Host "Pre-existing d3d9.dll backed up -> $StockBak" -ForegroundColor Yellow
    }
    Copy-Item $DxvkDll $Target -Force
    "DXVK $Version installed by tools/dxvk.ps1" | Set-Content $Marker
    Write-Host "DXVK $Version enabled." -ForegroundColor Green
    Write-Host "SINGLEPLAYER ONLY - PunkBuster ships with this game." -ForegroundColor Yellow
    Show-Status
}

if ($Disable) {
    Stop-IfRunning
    if (Test-Path $Target) {
        if (Test-Path $Marker) {
            Remove-Item $Target -Force
            Remove-Item $Marker -Force
            Write-Host "DXVK removed." -ForegroundColor Green
        } else {
            Write-Host "d3d9.dll present but not installed by this script - leaving it alone." -ForegroundColor Yellow
            exit 1
        }
    } else { Write-Host "Nothing to remove." }
    if (Test-Path $StockBak) {
        Copy-Item $StockBak $Target -Force
        Remove-Item $StockBak -Force
        Write-Host "Restored original d3d9.dll from backup."
    }
    Show-Status
}
