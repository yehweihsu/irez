# setup-kimi-plugin.ps1 — configure IREZ as a Kimi (Kimi Work desktop) plugin.
#
# Works from a source tree (build\ + mcp\target\release\) or an extracted
# release bundle (bin\). Generates a ready-to-install Kimi personal plugin
# (manifest + .mcp.json + bundled irez-investigation skill) with absolute
# paths from THIS machine, merges the stdio server into Kimi's runtime
# mcp.json, registers the plugin into the personal market when kimi-daimon
# is available, and finishes with `irez-mcp doctor --stdio`.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\setup-kimi-plugin.ps1 `
#       [-StateDir DIR] [-OutDir DIR] [-McpJson FILE] [-NoRegister] [-NoMcpJson]
#
# After it succeeds: open Kimi desktop -> Plugins -> "Personal" tab -> install
# "IREZ IR Evidence", then start a new session (mcp.json is read at session
# start).
[CmdletBinding()]
param(
  [string]$StateDir = "",
  [string]$OutDir = "",
  [string]$McpJson = "",
  [switch]$NoRegister,
  [switch]$NoMcpJson
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---- locate root and binaries ------------------------------------------------
$Root = Split-Path -Parent $PSScriptRoot

if (Test-Path (Join-Path $Root "bin\irez-mcp.exe")) {
  $McpBin = Join-Path $Root "bin\irez-mcp.exe"
  $CliBin = Join-Path $Root "bin\irez.exe"
} else {
  $McpBin = Join-Path $Root "mcp\target\release\irez-mcp.exe"
  $CliBin = Join-Path $Root "build\irez.exe"
}
foreach ($b in @($McpBin, $CliBin)) {
  if (-not (Test-Path $b)) {
    Write-Error @"
$b not found.
  source tree: build first (.\scripts\build-cpp.ps1; .\scripts\build-mcp.ps1)
  bundle: run this script from the extracted bundle root
"@
  }
}

# ---- version (contract.json is the single source of truth) --------------------
$Version = "0.1.0"
$Contract = Join-Path $Root "contract.json"
if (Test-Path $Contract) {
  $v = (Get-Content $Contract -Raw | ConvertFrom-Json).irez_version
  if ($v) { $Version = $v }
}

# ---- state dir ------------------------------------------------------------------
if (-not $StateDir) {
  $base = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $HOME "AppData\Local" }
  $StateDir = Join-Path $base "irez\kimi"
}
New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
# Initialize only when empty; never reindex an existing store behind the user's back.
if (-not (Test-Path (Join-Path $StateDir "investigation.sqlite"))) {
  & $CliBin --state-dir $StateDir init --name default | Out-Null
  Write-Host "state initialized: $StateDir (ingest artifacts with: irez --state-dir `"$StateDir`" ingest llvm <file.ll> --index full)"
} else {
  Write-Host "state exists: $StateDir (kept as-is)"
}

# ---- skill source -----------------------------------------------------------------
$SkillSrc = ""
foreach ($cand in @((Join-Path $Root "skills\irez-investigation"),
                    (Join-Path (Split-Path -Parent $Root) "skills\irez-investigation"))) {
  if (Test-Path (Join-Path $cand "SKILL.md")) { $SkillSrc = $cand; break }
}

# ---- generate plugin dir ------------------------------------------------------------
if (-not $OutDir) { $OutDir = Join-Path $Root "dist\kimi-plugin\irez" }
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "skills") | Out-Null

$mcpServers = [ordered]@{
  irez = [ordered]@{
    command = $McpBin
    args    = @()
    env     = [ordered]@{
      IREZ_CLI       = $CliBin
      IREZ_STATE_DIR = $StateDir
    }
  }
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-Utf8NoBom([string]$Path, [string]$Text) {
  [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

@{ mcpServers = $mcpServers } | ConvertTo-Json -Depth 8 | ForEach-Object {
  Write-Utf8NoBom (Join-Path $OutDir ".mcp.json") $_
}

$manifest = [ordered]@{
  '$schema'          = "https://catalog.msh.team/misc/kimi.plugin.schema.json"
  name               = "irez"
  version            = $Version
  description        = "Bounded, provenance-preserving LLVM IR evidence index: query functions, CFG/slices, guards, and trace-return evidence through 14 stdio MCP tools backed by the irez CLI."
  keywords           = @("llvm", "ir", "static-analysis", "mcp", "evidence", "cfg", "compiler")
  author             = "yehweihsu"
  homepage           = "https://github.com/yehweihsu/irez"
  license            = "Apache-2.0"
  skillInstructions  = "Use the IREZ MCP tools when the user asks to investigate LLVM IR artifacts or functions: locating functions or instructions, inspecting operands/uses, CFG structure, slices, guards/control-dependence evidence, trace-return analysis, or provenance-tracked evidence with explicit capability reporting. The tools are query-only and operate on a pre-ingested state directory; if a tool reports state not found or no artifacts, prepare the state with the irez CLI (init + ingest) first, then retry. Follow the bundled irez-investigation skill for bounded query discipline: never dump whole functions, report evidence with provenance, and state capability limits honestly."
  interface          = [ordered]@{
    displayName      = "IREZ IR Evidence"
    shortDescription = "LLVM IR evidence index with provenance, exposed as 14 stdio MCP query tools."
    longDescription  = "Bounded, provenance-preserving LLVM IR evidence index: query functions, CFG/slices, guards, and trace-return evidence through 14 stdio MCP tools backed by the irez CLI."
    developerName    = "yehweihsu"
    websiteURL       = "https://github.com/yehweihsu/irez"
    category         = "DEVELOPER"
    hostKind         = "local"
  }
  skills             = "./skills/"
  mcpServers         = $mcpServers
}
Write-Utf8NoBom (Join-Path $OutDir "kimi.plugin.json") ($manifest | ConvertTo-Json -Depth 8)

if ($SkillSrc) {
  $dst = Join-Path $OutDir "skills\irez-investigation"
  if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
  Copy-Item -Recurse $SkillSrc $dst
  Write-Host "skill bundled: $SkillSrc"
} else {
  Write-Warning "skills\irez-investigation not found next to the script; plugin generated without the skill"
}
Write-Host "plugin generated: $OutDir"

# ---- merge Kimi runtime mcp.json -----------------------------------------------------
if (-not $NoMcpJson) {
  if (-not $McpJson) {
    $appData = if ($env:APPDATA) { $env:APPDATA } else { Join-Path $HOME "AppData\Roaming" }
    $McpJson = Join-Path $appData "kimi-desktop\daimon-share\daimon\runtime\kimi-code\home\mcp.json"
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $McpJson) | Out-Null
  $data = [ordered]@{ mcpServers = [ordered]@{} }
  if (Test-Path $McpJson) {
    try {
      $existing = Get-Content $McpJson -Raw | ConvertFrom-Json
      if ($existing.mcpServers) {
        $existing.mcpServers.PSObject.Properties | ForEach-Object {
          $data.mcpServers[$_.Name] = $_.Value
        }
      }
    } catch {
      $backup = "$McpJson.bak"
      Move-Item $McpJson $backup
      Write-Warning "existing mcp.json was not valid JSON; backed up to $backup"
    }
  }
  $data.mcpServers["irez"] = $mcpServers.irez
  Write-Utf8NoBom $McpJson ($data | ConvertTo-Json -Depth 8)
  Write-Host "mcp.json updated: $McpJson"
}

# ---- register into the personal plugin market (optional) ------------------------------
if (-not $NoRegister) {
  $daimon = $env:DAIMON_RUNTIME_BINARY_PATH
  if (-not $daimon) { $daimon = (Get-Command kimi-daimon -ErrorAction SilentlyContinue).Source }
  if ($daimon -and (Test-Path $daimon)) {
    $argv = @("kimi-plugin", "register-personal", $OutDir, "--json")
    if ($env:KIMI_SHARE_DIR) { $argv += @("--share-dir", $env:KIMI_SHARE_DIR) }
    & $daimon @argv
    if ($LASTEXITCODE -eq 0) {
      Write-Host "registered into personal plugin market (install from the 'Personal' tab in Kimi desktop)"
    } else {
      Write-Warning "kimi-daimon registration failed; register later or install by hand"
    }
  } else {
    Write-Host "note: kimi-daimon not found; to register the plugin into the personal market run:"
    Write-Host "  kimi-daimon kimi-plugin register-personal `"$OutDir`" --json"
    Write-Host "or use the Kimi desktop plugin page. The mcp.json entry above already enables the tools."
  }
}

# ---- verify ----------------------------------------------------------------------------
$env:IREZ_CLI = $CliBin
$env:IREZ_STATE_DIR = $StateDir
& $McpBin doctor --stdio
if ($LASTEXITCODE -ne 0) { Write-Error "irez-mcp doctor failed" }

Write-Host ""
Write-Host "Done. Next steps:"
Write-Host "  1. Kimi desktop -> Plugins -> 'Personal' tab -> install 'IREZ IR Evidence' (if registered)."
Write-Host "  2. Start a NEW Kimi session (mcp.json is read at session start)."
Write-Host "  3. Ingest before querying: `"$CliBin`" --state-dir `"$StateDir`" ingest llvm <file.ll> --index full"
