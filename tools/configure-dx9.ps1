# Configures BFBC2's settings.ini for VR development work.
#
#   DxVersion  -> 9      the entire mod architecture targets the DX9 path
#   Fullscreen -> false  exclusive fullscreen on DX9 breaks Alt-Tab and the
#                        RenderDoc overlay; windowed is required for capture work
#
# The game REWRITES settings.ini on exit, so this waits for the process to close
# before touching the file. Safe to re-run; backs up once per run.
#
# Usage:  pwsh -File tools\configure-dx9.ps1
#         pwsh -File tools\configure-dx9.ps1 -Fullscreen   (restore fullscreen)

param(
    [switch]$Fullscreen,
    [int]$Width  = 1280,
    [int]$Height = 720
)

$ErrorActionPreference = 'Stop'

# Documents may be OneDrive-redirected - resolve the real path, don't assume.
$docs = [Environment]::GetFolderPath('MyDocuments')
$ini  = Join-Path $docs "BFBC2\settings.ini"

if (-not (Test-Path $ini)) {
    Write-Host "settings.ini not found at $ini" -ForegroundColor Red
    Write-Host "Launch the game once and reach the main menu to generate it."
    exit 1
}

# Wait for the game to close, or it will overwrite whatever we write.
$proc = Get-Process -Name BFBC2Game -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "BFBC2Game.exe is running (pid $($proc.Id)). Waiting for it to exit..." -ForegroundColor Yellow
    $proc.WaitForExit()
    Start-Sleep -Seconds 3   # let the game flush its final write
    Write-Host "Game closed."
}

$backup = "$ini.bak"
if (-not (Test-Path $backup)) {
    Copy-Item $ini $backup
    Write-Host "Backed up original -> $backup"
}

$fsValue = if ($Fullscreen) { 'true' } else { 'false' }

$lines = Get-Content $ini
$out = foreach ($line in $lines) {
    switch -Regex ($line) {
        '^DxVersion='  { 'DxVersion=9';           continue }
        '^Fullscreen=' { "Fullscreen=$fsValue";   continue }
        '^Width='      { "Width=$Width";          continue }
        '^Height='     { "Height=$Height";        continue }
        default        { $line }
    }
}
Set-Content -Path $ini -Value $out -Encoding ASCII

Write-Host "`nApplied:" -ForegroundColor Green
Get-Content $ini | Where-Object { $_ -match '^(DxVersion|Fullscreen|Width|Height|Fov|VSync)=' }

Write-Host "`nNext: relaunch and confirm RenderDoc reports the API as D3D9, not D3D11." -ForegroundColor Cyan
