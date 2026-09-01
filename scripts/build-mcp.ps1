[CmdletBinding()]
param([switch]$DebugBuild)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-env.ps1")
Enter-IrezVsDevShell
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
    if (Test-Path (Join-Path $cargoBin "cargo.exe")) { $env:Path = "$cargoBin;$env:Path" }
}
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw "Cargo was not found; install Rust 1.88 or newer" }
Push-Location (Join-Path $PSScriptRoot "..\mcp")
try {
    $arguments = @("build", "--locked")
    if (-not $DebugBuild) { $arguments += "--release" }
    & cargo @arguments
    exit $LASTEXITCODE
} finally { Pop-Location }
