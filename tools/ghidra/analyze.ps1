# Headless Ghidra pass over BFBC2Game.exe.
#
#   pwsh -File tools\ghidra\analyze.ps1            # import + analyze + run both scripts
#   pwsh -File tools\ghidra\analyze.ps1 -NoImport  # re-run the scripts on the existing project
#
# Output lands in build\ghidra\out\:
#   frostbite_types.txt   type-name -> TypeInfoData -> TypeInfo chains, link-offset inference, root candidates
#   functions.csv         rva,size,name for every function (resolves census call-site RVAs)
#   known_xrefs.txt       who references the FOV object's vtable (its constructor)
#
# The Ghidra project itself is build\ghidra\bfbc2.gpr and is reusable from the GUI:
#   C:\tools\ghidra_12.1.3_PUBLIC\ghidraRun.bat  ->  open build\ghidra\bfbc2.gpr
#
# Singleplayer-only project; this reads the executable on disk and never touches a process.
param(
    [string]$Ghidra  = "C:\tools\ghidra_12.1.3_PUBLIC",
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Battlefield Bad Company 2",
    [switch]$NoImport
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Proj = Join-Path $Root "build\ghidra"
$Out  = Join-Path $Proj "out"
$Exe  = Join-Path $GameDir "BFBC2Game.exe"
$Headless = Join-Path $Ghidra "support\analyzeHeadless.bat"

if (-not (Test-Path $Headless)) { throw "analyzeHeadless not found at $Headless" }
if (-not (Test-Path $Exe))      { throw "game exe not found at $Exe" }
New-Item -ItemType Directory -Force $Proj | Out-Null
New-Item -ItemType Directory -Force $Out  | Out-Null

# Ghidra 12 needs a JDK 21+. Prefer JAVA_HOME; otherwise find Temurin.
if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -like "jdk-21*" } | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName; $env:PATH = "$($jdk.FullName)\bin;$env:PATH" }
}
Write-Host "JAVA_HOME = $env:JAVA_HOME"

$common = @($Proj, "bfbc2", "-scriptPath", $PSScriptRoot,
            "-postScript", "FindFrostbiteTypes.java", $Out,
            "-postScript", "ExportFunctions.java", $Out)

if ($NoImport) {
    & $Headless @common -process "BFBC2Game.exe" -noanalysis
} else {
    # analyzeHeadless.bat is a cmd script, and cmd cannot pass a path containing
    # "(x86)" through its argument parsing intact. Stage a copy of the exe under
    # the project directory (no spaces, no parens) and import that. The install
    # is only ever read.
    $Staged = Join-Path $Proj "BFBC2Game.exe"
    Copy-Item $Exe $Staged -Force
    # -overwrite: re-import replaces a stale program of the same name.
    & $Headless @common -import $Staged -overwrite
}
if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless exited $LASTEXITCODE" }
Write-Host "`nOutputs:"; Get-ChildItem $Out | Format-Table Name, Length, LastWriteTime -AutoSize
