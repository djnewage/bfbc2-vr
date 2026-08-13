# Watches the proxy log live while the game runs.
#
# Opens with FileShare.ReadWrite so it does not fight the game's own handle.
# Requires a build that opens the log with _fsopen/_SH_DENYWR - see logger.cpp.
#
#   pwsh -File tools\tail-log.ps1
#   pwsh -File tools\tail-log.ps1 -FromStart

param(
    [switch]$FromStart,
    [int]$PollMs = 400
)

$log = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2\bfbc2vr.log"

Write-Host "Tailing $log" -ForegroundColor Cyan
Write-Host "Ctrl+C to stop.`n"

while (-not (Test-Path $log)) {
    Write-Host "waiting for log to appear..." -ForegroundColor DarkGray
    Start-Sleep -Seconds 2
}

$fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    if (-not $FromStart) { $fs.Seek(0, [System.IO.SeekOrigin]::End) | Out-Null }
    $reader = New-Object System.IO.StreamReader($fs)

    while ($true) {
        $line = $reader.ReadLine()
        if ($null -eq $line) { Start-Sleep -Milliseconds $PollMs; continue }

        $color = switch -Regex ($line) {
            'FATAL|FAILED|\*\*\*' { 'Red' }
            'WARNING|NOTE'        { 'Yellow' }
            '\[constants\]'       { 'Green' }
            '\[device\]'          { 'Cyan' }
            default               { 'Gray' }
        }
        Write-Host $line -ForegroundColor $color
    }
}
finally { $fs.Dispose() }
