[CmdletBinding()]
param(
    [string]$BuildDir = $(if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "" }),
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = $(if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { "RelWithDebInfo" }),
    [string]$LlvmDir = $env:IREZ_LLVM_DIR,
    [string]$DepsDir = $env:IREZ_DEPS_DIR,
    [switch]$NoTests
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-env.ps1")
Enter-IrezVsDevShell
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
if (-not $DepsDir) { $DepsDir = Join-Path (Split-Path $root -Parent) "Software_Repos" }
if (-not $LlvmDir) {
    $LlvmDir = Get-ChildItem -Path $DepsDir -Directory -Filter "LLVM-*" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "lib\cmake\llvm\LLVMConfig.cmake") } |
        Select-Object -First 1 -ExpandProperty FullName
}
$configure = @("-S", $root, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=$Configuration",
               "-DIREZ_BUILD_TESTS=$(([string](!$NoTests.IsPresent)).ToUpperInvariant())")
if ($DepsDir) { $configure += "-DIREZ_DEPS_DIR=$DepsDir" }
if ($LlvmDir) { $configure += "-DLLVM_DIR=$(Join-Path $LlvmDir 'lib\cmake\llvm')" }
if (Get-Command ninja -ErrorAction SilentlyContinue) { $configure += @("-G", "Ninja") }
& cmake @configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $BuildDir --config $Configuration --parallel 2
exit $LASTEXITCODE
