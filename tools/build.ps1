# Builds, installs, and removes the x86 proxy DLL.
#
#   pwsh -File tools\build.ps1                 build only
#   pwsh -File tools\build.ps1 -Install        build + deploy into the game dir
#   pwsh -File tools\build.ps1 -Uninstall      restore the game to stock
#   pwsh -File tools\build.ps1 -Clean          wipe the build directory first
#
# Run -Uninstall before touching multiplayer. PunkBuster ships with this game.

param(
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$Clean,
    # Override if the game is not in the default Steam location.
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2"
)

$ErrorActionPreference = 'Stop'

$Root     = Split-Path $PSScriptRoot -Parent
$Build    = Join-Path $Root 'build'
$Target   = Join-Path $GameDir 'd3d9.dll'
# The SAME binary is installed a second time under the dinput8 name. BFBC2Game
# imports dinput8 and has no raw input at all, so that is where controller
# input is injected; the DLL picks its role from its own filename at load.
$DInputTarget = Join-Path $GameDir 'dinput8.dll'
$DxvkName = Join-Path $GameDir 'dxvk_d3d9.dll'

# --- identity -------------------------------------------------------------
# Identify a DLL by its contents, never by a sidecar marker file. Markers
# desync the moment something is installed by a script version predating them,
# and then an ordinary reinstall looks like a stranger's DLL and gets refused.
# Our proxy compiles in BFBC2VR_PROXY_MAGIC_v1; a binary cannot lie about itself.
function Get-DllKind([string]$path) {
    if (-not (Test-Path $path)) { return 'absent' }
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path))

    # Order matters. Our proxy contains the literal "dxvk_d3d9.dll" because it
    # LoadLibrarys DXVK, so any DXVK test must run AFTER we rule ourselves out
    # or the proxy gets misidentified as the thing it loads.
    if ($text.Contains('BFBC2VR_PROXY_MAGIC_v1')) { return 'ours' }
    if ($text.Contains('bfbc2vr'))                { return 'ours' }  # builds predating the magic string

    # DXVK-internal symbols. Deliberately not matching "DXVK" or "dxvk_d3d9",
    # both of which appear in our own log strings.
    if ($text -match 'DxvkInstance|dxvk::|DXVK_CONFIG_FILE|DxvkDevice') { return 'dxvk' }
    return 'unknown'
}

function Remove-StaleCopies {
    Get-ChildItem $GameDir -Filter 'dinput8.dll.stale-*' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
    Get-ChildItem $GameDir -Filter 'd3d9.dll.stale-*' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
    Remove-Item (Join-Path $GameDir 'd3d9.dll.dxvk-installed')    -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GameDir 'd3d9.dll.bfbc2vr-installed') -Force -ErrorAction SilentlyContinue
}

# --- uninstall ------------------------------------------------------------
if ($Uninstall) {
    if (Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue) {
        throw "BFBC2Game.exe is running. Close it first."
    }

    switch (Get-DllKind $Target) {
        'ours'    { Remove-Item $Target -Force; Write-Host "Removed bfbc2vr proxy." -ForegroundColor Green }
        'dxvk'    { Write-Host "d3d9.dll is DXVK, not ours - left in place." -ForegroundColor Cyan }
        'unknown' { Write-Host "d3d9.dll is unrecognized - left alone." -ForegroundColor Yellow }
        'absent'  { Write-Host "No d3d9.dll present." }
    }

    # Only ever remove a dinput8.dll that is provably ours. A genuine Microsoft
    # one or another mod's proxy both read as 'unknown', and there is nothing to
    # chain to here, so displacing a stranger's file would be pure damage.
    switch (Get-DllKind $DInputTarget) {
        'ours'    { Remove-Item $DInputTarget -Force; Write-Host "Removed bfbc2vr dinput8 proxy." -ForegroundColor Green }
        'absent'  { }
        default   { Write-Host "dinput8.dll is not ours - left alone." -ForegroundColor Yellow }
    }

    # Put DXVK back where dxvk.ps1 expects to find it.
    if ((Test-Path $DxvkName) -and -not (Test-Path $Target)) {
        Move-Item $DxvkName $Target -Force
        "DXVK restored by tools/build.ps1 -Uninstall" | Set-Content (Join-Path $GameDir 'd3d9.dll.dxvk-installed')
        Write-Host "Restored DXVK to d3d9.dll. Run tools\dxvk.ps1 -Disable for fully stock." -ForegroundColor Cyan
    }

    Remove-StaleCopies
    Write-Host "`nVerify with: pwsh -File tools\dxvk.ps1 -Status"
    exit 0
}

# --- build ----------------------------------------------------------------
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

# Confirm the architecture rather than trusting the generator.
$bytes   = [System.IO.File]::ReadAllBytes($dll)
$peOff   = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
$arch    = switch ($machine) { 0x14c { 'x86' } 0x8664 { 'x64' } default { "0x{0:X}" -f $machine } }

Write-Host "`nBuilt: $dll"
Write-Host "Arch : $arch" -ForegroundColor $(if ($arch -eq 'x86') { 'Green' } else { 'Red' })
if ($arch -ne 'x86') { throw "wrong architecture - BFBC2Game.exe is 32-bit" }

# Sanity: the build must be self-identifying, or install-time detection breaks.
if ((Get-DllKind $dll) -ne 'ours') { throw "built DLL lacks its magic string - check src/d3d9.def exports BFBC2VR_ProxyVersion" }

if (-not $Install) {
    Write-Host "`nRe-run with -Install to deploy into the game directory." -ForegroundColor Cyan
    exit 0
}

# --- install --------------------------------------------------------------
$kind = Get-DllKind $Target
Write-Host "`nExisting d3d9.dll: $kind" -ForegroundColor DarkGray

if ($kind -eq 'unknown') {
    throw "d3d9.dll present but unrecognized (neither ours nor DXVK) - refusing to overwrite. Inspect it first."
}
if ($kind -eq 'dxvk') {
    # Never delete an existing dxvk_d3d9.dll to make room - it may be mapped by
    # a running game, and it is the backend we depend on. Refuse instead.
    if (Test-Path $DxvkName) {
        throw "d3d9.dll looks like DXVK but dxvk_d3d9.dll already exists. Two DXVK copies - resolve manually before installing."
    }
    Move-Item $Target $DxvkName -Force
    Write-Host "Moved DXVK aside -> dxvk_d3d9.dll (we chain in front of it)" -ForegroundColor Yellow
}

# A loaded DLL cannot be overwritten, but it can usually be renamed - the
# running process keeps its existing mapping and the next launch picks up the
# new file. Saves a close-the-game round trip. If Windows refuses, say so.
$running = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
$needsRestart = $false
if ($running -and (Test-Path $Target)) {
    $stale = Join-Path $GameDir ("d3d9.dll.stale-" + [System.IO.Path]::GetRandomFileName().Substring(0,6))
    try {
        Move-Item $Target $stale -Force -ErrorAction Stop
        $needsRestart = $true
        Write-Host "Game is running - renamed the loaded DLL aside." -ForegroundColor Yellow
    } catch {
        throw "BFBC2Game.exe is running (pid $($running.Id)) and its d3d9.dll could not be renamed. Close the game and retry."
    }
}

Copy-Item $dll $Target -Force
Write-Host "Installed proxy -> $Target" -ForegroundColor Green

# --- input proxy -----------------------------------------------------------
# Same binary, second name. Refuse to overwrite anything we did not write: a
# real dinput8 or another mod's proxy both classify as 'unknown', and a partial
# install is still a working install because the input path falls back to
# SendInput when the wrappers do not report in.
$dinputKind = Get-DllKind $DInputTarget
if ($dinputKind -eq 'ours' -or $dinputKind -eq 'absent') {
    if ($running -and (Test-Path $DInputTarget)) {
        $staleDi = Join-Path $GameDir ("dinput8.dll.stale-" + [System.IO.Path]::GetRandomFileName().Substring(0,6))
        try {
            Move-Item $DInputTarget $staleDi -Force -ErrorAction Stop
            $needsRestart = $true
        } catch {
            Write-Host "dinput8.dll is loaded and could not be renamed - close the game to update it." -ForegroundColor Yellow
        }
    }
    if (-not (Test-Path $DInputTarget)) {
        Copy-Item $dll $DInputTarget -Force
        Write-Host "Installed input proxy -> $DInputTarget" -ForegroundColor Green
    }
} else {
    Write-Host "dinput8.dll exists and is NOT ours - left untouched. Controller input will use the SendInput fallback." -ForegroundColor Yellow
}

# OpenVR runtime DLL (x86) - required since the proxy links openvr_api.lib.
$openvrDll = Join-Path $Root 'thirdparty\openvr\bin\win32\openvr_api.dll'
if (Test-Path $openvrDll) {
    Copy-Item $openvrDll (Join-Path $GameDir 'openvr_api.dll') -Force
    Write-Host "Installed openvr_api.dll (x86)" -ForegroundColor Green
}

if (Test-Path $DxvkName) {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> dxvk_d3d9.dll -> Vulkan" -ForegroundColor Green
} else {
    Write-Host "Chain: BFBC2Game.exe -> d3d9.dll (ours) -> system D3D9" -ForegroundColor Yellow
    Write-Host "  (run tools\dxvk.ps1 -Enable first if you want the Vulkan backend)"
}

if (-not $running) { Remove-StaleCopies }
if ($needsRestart) { Write-Host "`nRESTART THE GAME to load this build." -ForegroundColor Yellow }

Write-Host "`nSINGLEPLAYER ONLY - PunkBuster ships with this game." -ForegroundColor Yellow
Write-Host "Log: $GameDir\bfbc2vr.log"
