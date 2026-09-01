[CmdletBinding()]
param(
    [string]$BuildDir = $(if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "" }),
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = $(if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { "RelWithDebInfo" })
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-env.ps1")
Enter-IrezVsDevShell
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
& ctest --test-dir $BuildDir -C $Configuration --output-on-failure
exit $LASTEXITCODE
