# Shared Visual Studio/MSVC environment discovery for IREZ PowerShell scripts.
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Enter-IrezVsDevShell {
    $candidates = @(@(
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
            "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
        ) | Where-Object { $_ -and (Test-Path $_) })
    if (-not $candidates) {
        throw "vswhere.exe was not found; install Visual Studio 2022 or newer with the Desktop development with C++ workload"
    }
    $vsPath = & $candidates[0] -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -prerelease
    if (-not $vsPath) { throw "no Visual Studio installation with the x64 C++ toolchain was found" }
    $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) { throw "VsDevCmd.bat is missing under $vsPath" }

    # VsDevCmd is a batch file. Capture the environment it creates and import
    # it into this PowerShell process; all actual orchestration remains here.
    & $env:ComSpec /s /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set" |
        ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                $name = $matches[1]
                $value = $matches[2]
                # A parent process can contain both PATH and Path entries.
                # VsDevCmd updates PATH, while the stale Path entry may appear
                # later in `set` output and otherwise overwrite the MSVC paths.
                if ($name -ieq 'PATH') {
                    if ($value -match '\\VC\\Tools\\MSVC\\') {
                        $env:Path = $value
                    }
                } else {
                    [Environment]::SetEnvironmentVariable($name, $value, 'Process')
                }
            }
        }
    if ($LASTEXITCODE -ne 0) { throw "VsDevCmd.bat failed with exit code $LASTEXITCODE" }

    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        $cmakeBin = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
        if (-not (Test-Path (Join-Path $cmakeBin "cmake.exe"))) { throw "cmake.exe was not found" }
        $env:Path = "$cmakeBin;$env:Path"
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        $ninjaBin = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
        if (Test-Path (Join-Path $ninjaBin "ninja.exe")) { $env:Path = "$ninjaBin;$env:Path" }
    }
}
