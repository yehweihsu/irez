. (Join-Path $PSScriptRoot 'windows-env.ps1')
Enter-IrezVsDevShell
$buildDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'build'
& cmake --build $buildDir --parallel 2
exit $LASTEXITCODE
