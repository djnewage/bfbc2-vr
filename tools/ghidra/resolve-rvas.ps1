# Annotate a draw-census dump with function names from Ghidra's functions.csv.
#
#   pwsh -File tools\ghidra\resolve-rvas.ps1 "<game dir>\bfbc2vr_draws_<stamp>_00.txt"
#
# The census logs "ret=<rva>" and "stack=<rva>[,<rva>...]" relative to the image
# base it prints in its header. This maps each RVA to the containing function,
# so the engine's draw-submission call sites get names instead of numbers.
# Writes <input>.resolved.txt next to the input.
param(
    [Parameter(Mandatory)][string]$Census,
    [string]$Functions = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "build\ghidra\out\functions.csv")
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Functions)) { throw "functions.csv not found at $Functions - run tools\ghidra\analyze.ps1 first" }

# Sorted (rva, size, name) so an RVA resolves by binary search to the function containing it.
$funcs = Get-Content $Functions | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
    $p = $_.Split(',', 3)
    [pscustomobject]@{ Rva = [Convert]::ToInt64($p[0], 16); Size = [int64]$p[1]; Name = $p[2] }
} | Sort-Object Rva
$rvas = [int64[]]($funcs | ForEach-Object Rva)

function Resolve-Rva([int64]$rva) {
    $lo = 0; $hi = $rvas.Length - 1; $best = -1
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        if ($rvas[$mid] -le $rva) { $best = $mid; $lo = $mid + 1 } else { $hi = $mid - 1 }
    }
    if ($best -lt 0) { return "?" }
    $f = $funcs[$best]
    if ($rva -ge $f.Rva + $f.Size) { return ("?+" + ('{0:x}' -f ($rva - $f.Rva))) }
    return ("{0}+{1:x}" -f $f.Name, ($rva - $f.Rva))
}

$out = [System.IO.Path]::ChangeExtension($Census, ".resolved.txt")
$cache = @{}
Get-Content $Census | ForEach-Object {
    $line = $_
    $line = [regex]::Replace($line, '(ret|stack)=([0-9a-fA-F]+(?:,[0-9a-fA-F]+)*)', {
        param($m)
        $names = $m.Groups[2].Value.Split(',') | ForEach-Object {
            $r = [Convert]::ToInt64($_, 16)
            if (-not $cache.ContainsKey($r)) { $cache[$r] = Resolve-Rva $r }
            "$_=" + $cache[$r]
        }
        "$($m.Groups[1].Value)=" + ($names -join ',')
    })
    $line
} | Set-Content $out -Encoding UTF8
Write-Host "wrote $out  ($($cache.Count) distinct RVAs resolved)"
