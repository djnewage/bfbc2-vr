# Launches BFBC2 and reports which graphics backend actually went live.
#
# Answers, without a human looking at the screen:
#   - is the DX9 path active (vs DX10/11)?
#   - is d3d9.dll resolving to our game-directory DLL or to System32?
#   - did Vulkan / the DXVK backend come up?
#
#   pwsh -File tools\verify-renderer.ps1
#   pwsh -File tools\verify-renderer.ps1 -WaitForExit   (wait for a running game to close first)

param(
    [switch]$WaitForExit,
    [int]$InitSeconds = 20,
    [int]$LaunchTimeoutSeconds = 120
)

$ErrorActionPreference = 'Continue'
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2"

$running = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
if ($running) {
    if ($WaitForExit) {
        Write-Host "Waiting for running game (pid $($running.Id)) to close..." -ForegroundColor Yellow
        $running.WaitForExit(); Start-Sleep -Seconds 3
    } else {
        Write-Host "Game already running (pid $($running.Id)). Use -WaitForExit to restart cleanly." -ForegroundColor Red
        exit 1
    }
}

Write-Host "Launching BFBC2..." -ForegroundColor Cyan
Start-Process "steam://rungameid/24960"

$deadline = (Get-Date).AddSeconds($LaunchTimeoutSeconds)
$proc = $null
while ((Get-Date) -lt $deadline) {
    $proc = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
    if ($proc) { break }
    Start-Sleep -Milliseconds 1500
}
if (-not $proc) { Write-Host "Game did not start within ${LaunchTimeoutSeconds}s." -ForegroundColor Red; exit 1 }

Write-Host "Started (pid $($proc.Id)). Waiting ${InitSeconds}s for renderer init..."
Start-Sleep -Seconds $InitSeconds
$proc.Refresh()

$mods = @()
try { $mods = $proc.Modules | Select-Object @{n='Name';e={$_.ModuleName.ToLower()}}, FileName }
catch { Write-Host "Module enumeration failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

Write-Host "`n=== Graphics / input modules ===" -ForegroundColor Cyan
$mods | Where-Object { $_.Name -match 'd3d|dxgi|vulkan|nv(d3dum|oglv)|dinput|xinput|renderdoc' } |
        Sort-Object Name | Format-Table Name, FileName -AutoSize

$names = $mods.Name
$d3d9  = $mods | Where-Object { $_.Name -eq 'd3d9.dll' } | Select-Object -First 1

Write-Host "=== Verdict ===" -ForegroundColor Cyan

if ($d3d9) {
    $fromGameDir = $d3d9.FileName -like "$GameDir*"
    if ($fromGameDir) {
        Write-Host "d3d9.dll loaded from GAME DIR - proxy/DXVK is in the loop" -ForegroundColor Green
        Write-Host "  $($d3d9.FileName)"
    } else {
        Write-Host "d3d9.dll loaded from SYSTEM - stock Microsoft D3D9" -ForegroundColor Yellow
        Write-Host "  $($d3d9.FileName)"
    }
} else {
    Write-Host "d3d9.dll NOT loaded - the game is not on the DX9 path" -ForegroundColor Red
}

if ($names -contains 'd3d11.dll') { Write-Host "d3d11.dll loaded - DX11 path is active, check DxVersion in settings.ini" -ForegroundColor Red }

$vulkan = $names | Where-Object { $_ -match '^vulkan|nvoglv' }
if ($vulkan) {
    Write-Host "Vulkan backend ACTIVE ($($vulkan -join ', ')) - DXVK is translating" -ForegroundColor Green
} else {
    Write-Host "No Vulkan modules - native D3D9, DXVK not in use" -ForegroundColor Yellow
}

Write-Host "`nGame left running (pid $($proc.Id))."
