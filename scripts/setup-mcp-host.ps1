# setup-mcp-host.ps1 -- one-shot irez-mcp host registration (Windows).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\setup-mcp-host.ps1 [-Agent opencode|codex]
#
# Builds whatever is missing through the PowerShell/MSVC entry points,
# registers the MCP server with the chosen host,
# links the irez-investigation skill, and verifies the chain with `doctor`.
# See docs/MCP_SETUP.md.
param(
  [ValidateSet("opencode", "codex")]
  [string]$Agent = "opencode",
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$ExtraArgs
)

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $Root "build" }
$Cli  = Join-Path $BuildDir "irez.exe"
$Mcp  = Join-Path $Root "mcp\target\release\irez-mcp.exe"

if (-not (Test-Path $Cli)) {
  Write-Host "== CLI not built; running scripts\build-cpp.ps1 =="
  & (Join-Path $PSScriptRoot "build-cpp.ps1")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if (-not (Test-Path $Mcp)) {
  Write-Host "== irez-mcp not built; running scripts\build-mcp.ps1 =="
  & (Join-Path $PSScriptRoot "build-mcp.ps1")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "== Registering irez-mcp with $Agent =="
& $Mcp install $Agent --cli $Cli @ExtraArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== Verifying =="
& $Mcp doctor $Agent --cli $Cli
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$StateDir = Join-Path $env:LOCALAPPDATA "irez\$Agent"
Write-Host ""
Write-Host "Done. State dir: $StateDir (override with --state-dir)."
Write-Host "Prepare it with the CLI before querying, e.g.:"
Write-Host "  $Cli --state-dir $StateDir ingest llvm $Root\fixtures\nonfloating.ll --index full"
