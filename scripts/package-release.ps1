[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = $(if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "" }),
    [string]$CargoTargetDir = $(if ($env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR } else { "" }),
    [string]$OutputDir = ""
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
if (-not $CargoTargetDir) { $CargoTargetDir = Join-Path $root "mcp\target" }
if (-not $OutputDir) { $OutputDir = Join-Path $root "dist" }
& (Join-Path $PSScriptRoot "build-cpp.ps1") -BuildDir $BuildDir -Configuration Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot "run-tests.ps1") -BuildDir $BuildDir -Configuration Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot "build-mcp.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$cppBin = if (Test-Path (Join-Path $BuildDir "irez.exe")) { $BuildDir } else { Join-Path $BuildDir "Release" }
$mcpBin = Join-Path $CargoTargetDir "release\irez-mcp.exe"
& python (Join-Path $PSScriptRoot "package_release.py") --platform windows-x86_64 --version $Version --cpp-bin $cppBin --mcp-bin $mcpBin --output $OutputDir
exit $LASTEXITCODE
