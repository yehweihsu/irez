#!/usr/bin/env bash
# setup-kimi-plugin.sh — configure IREZ as a Kimi (Kimi Work desktop) plugin.
#
# Works from a source tree (build/ + mcp/target/release/) or an extracted
# release bundle (bin/). Generates a ready-to-install Kimi personal plugin
# (manifest + .mcp.json + bundled irez-investigation skill) with absolute
# paths from THIS machine, merges the stdio server into Kimi's runtime
# mcp.json, registers the plugin into the personal market when kimi-daimon
# is available, and finishes with `irez-mcp doctor --stdio`.
#
# Usage:
#   scripts/setup-kimi-plugin.sh [--state-dir DIR] [--out DIR]
#                                [--mcp-json FILE] [--no-register] [--no-mcp-json]
#
# After it succeeds: open Kimi desktop -> Plugins -> "Personal" tab -> install
# "IREZ IR Evidence", then start a new session (mcp.json is read at session
# start).
set -euo pipefail

# ---- args ------------------------------------------------------------------
STATE_DIR=""
OUT_DIR=""
MCP_JSON=""
DO_REGISTER=1
DO_MCP_JSON=1
while [ $# -gt 0 ]; do
  case "$1" in
    --state-dir)   STATE_DIR="$2"; shift 2 ;;
    --out)         OUT_DIR="$2"; shift 2 ;;
    --mcp-json)    MCP_JSON="$2"; shift 2 ;;
    --no-register) DO_REGISTER=0; shift ;;
    --no-mcp-json) DO_MCP_JSON=0; shift ;;
    -h|--help)     grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# ---- locate root and binaries ----------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

is_windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) is_windows=1 ;; esac

exe=""
[ "$is_windows" = 1 ] && exe=".exe"

# Release bundle layout: <root>/bin/irez-mcp ; source tree: mcp/target/release.
if [ -x "$ROOT/bin/irez-mcp$exe" ]; then
  MCP_BIN="$ROOT/bin/irez-mcp$exe"
  CLI_BIN="$ROOT/bin/irez$exe"
else
  MCP_BIN="$ROOT/mcp/target/release/irez-mcp$exe"
  CLI_BIN="$ROOT/build/irez$exe"
fi
for b in "$MCP_BIN" "$CLI_BIN"; do
  if [ ! -x "$b" ]; then
    echo "error: $b not found or not executable." >&2
    echo "  source tree: build first (make build && make mcp, or scripts/build-cpp.ps1 + scripts/build-mcp.ps1 on Windows)" >&2
    echo "  bundle: run this script from the extracted bundle root" >&2
    exit 1
  fi
done

# ---- version (contract.json is the single source of truth) ------------------
VERSION="0.1.0"
if [ -f "$ROOT/contract.json" ]; then
  v="$(sed -n 's/.*"irez_version"[^"]*"\([^"]*\)".*/\1/p' "$ROOT/contract.json" | head -1)"
  [ -n "$v" ] && VERSION="$v"
fi

# Hosts are native Windows processes: under Git Bash/MSYS the resolved paths are
# POSIX-style (/d/...), which Kimi desktop cannot execute. Convert everything we
# embed into JSON to native Windows form. MSYS file ops accept both forms, so
# converting early is safe.
if [ "$is_windows" = 1 ] && command -v cygpath >/dev/null 2>&1; then
  MCP_BIN="$(cygpath -w "$MCP_BIN")"
  CLI_BIN="$(cygpath -w "$CLI_BIN")"
fi

# ---- state dir ----------------------------------------------------------------
if [ -z "$STATE_DIR" ]; then
  if [ "$is_windows" = 1 ]; then
    base="${LOCALAPPDATA:-$HOME/AppData/Local}"
  else
    base="${XDG_DATA_HOME:-$HOME/.local/share}"
  fi
  STATE_DIR="$base/irez/kimi"
fi
if [ "$is_windows" = 1 ] && command -v cygpath >/dev/null 2>&1; then
  STATE_DIR="$(cygpath -w "$STATE_DIR")"
fi
mkdir -p "$STATE_DIR"
# Initialize only when empty; never reindex an existing store behind the user's back.
if [ ! -f "$STATE_DIR/investigation.sqlite" ]; then
  "$CLI_BIN" --state-dir "$STATE_DIR" init --name default >/dev/null
  echo "state initialized: $STATE_DIR (ingest artifacts with: irez --state-dir \"$STATE_DIR\" ingest llvm <file.ll> --index full)"
else
  echo "state exists: $STATE_DIR (kept as-is)"
fi

# ---- skill source ---------------------------------------------------------------
SKILL_SRC=""
for cand in "$ROOT/skills/irez-investigation" "$ROOT/../skills/irez-investigation"; do
  [ -f "$cand/SKILL.md" ] && SKILL_SRC="$cand" && break
done

# ---- generate plugin dir --------------------------------------------------------
[ -z "$OUT_DIR" ] && OUT_DIR="$ROOT/dist/kimi-plugin/irez"
if [ "$is_windows" = 1 ] && command -v cygpath >/dev/null 2>&1; then
  OUT_DIR="$(cygpath -w "$OUT_DIR")"
fi
mkdir -p "$OUT_DIR/skills"

# JSON-escape backslashes for Windows paths.
jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g'; }
MCP_JSON_ESC="$(jesc "$MCP_BIN")"
CLI_JSON_ESC="$(jesc "$CLI_BIN")"
STATE_JSON_ESC="$(jesc "$STATE_DIR")"

MCP_SERVERS=$(cat <<EOF
{
    "irez": {
      "command": "$MCP_JSON_ESC",
      "args": [],
      "env": {
        "IREZ_CLI": "$CLI_JSON_ESC",
        "IREZ_STATE_DIR": "$STATE_JSON_ESC"
      }
    }
  }
EOF
)

cat > "$OUT_DIR/.mcp.json" <<EOF
{
  "mcpServers": $MCP_SERVERS
}
EOF

cat > "$OUT_DIR/kimi.plugin.json" <<EOF
{
  "\$schema": "https://catalog.msh.team/misc/kimi.plugin.schema.json",
  "name": "irez",
  "version": "$VERSION",
  "description": "Bounded, provenance-preserving LLVM IR evidence index: query functions, CFG/slices, guards, and trace-return evidence through 14 stdio MCP tools backed by the irez CLI.",
  "keywords": ["llvm", "ir", "static-analysis", "mcp", "evidence", "cfg", "compiler"],
  "author": "yehweihsu",
  "homepage": "https://github.com/yehweihsu/irez",
  "license": "Apache-2.0",
  "skillInstructions": "Use the IREZ MCP tools when the user asks to investigate LLVM IR artifacts or functions: locating functions or instructions, inspecting operands/uses, CFG structure, slices, guards/control-dependence evidence, trace-return analysis, or provenance-tracked evidence with explicit capability reporting. The tools are query-only and operate on a pre-ingested state directory; if a tool reports state not found or no artifacts, prepare the state with the irez CLI (init + ingest) first, then retry. Follow the bundled irez-investigation skill for bounded query discipline: never dump whole functions, report evidence with provenance, and state capability limits honestly.",
  "interface": {
    "displayName": "IREZ IR Evidence",
    "shortDescription": "LLVM IR evidence index with provenance, exposed as 14 stdio MCP query tools.",
    "longDescription": "Bounded, provenance-preserving LLVM IR evidence index: query functions, CFG/slices, guards, and trace-return evidence through 14 stdio MCP tools backed by the irez CLI.",
    "developerName": "yehweihsu",
    "websiteURL": "https://github.com/yehweihsu/irez",
    "category": "DEVELOPER",
    "hostKind": "local"
  },
  "skills": "./skills/",
  "mcpServers": $MCP_SERVERS
}
EOF

if [ -n "$SKILL_SRC" ]; then
  rm -rf "$OUT_DIR/skills/irez-investigation"
  cp -r "$SKILL_SRC" "$OUT_DIR/skills/irez-investigation"
  echo "skill bundled: $SKILL_SRC"
else
  echo "warning: skills/irez-investigation not found next to the script; plugin generated without the skill" >&2
fi
echo "plugin generated: $OUT_DIR"

# ---- merge Kimi runtime mcp.json -------------------------------------------------
if [ "$DO_MCP_JSON" = 1 ]; then
  if [ -z "$MCP_JSON" ]; then
    case "$(uname -s)" in
      Darwin) MCP_JSON="$HOME/Library/Application Support/kimi-desktop/daimon-share/daimon/runtime/kimi-code/home/mcp.json" ;;
      MINGW*|MSYS*|CYGWIN*) MCP_JSON="${APPDATA:-$HOME/AppData/Roaming}/kimi-desktop/daimon-share/daimon/runtime/kimi-code/home/mcp.json" ;;
      *) MCP_JSON="${XDG_CONFIG_HOME:-$HOME/.config}/kimi-desktop/daimon-share/daimon/runtime/kimi-code/home/mcp.json" ;;
    esac
  fi
  mkdir -p "$(dirname "$MCP_JSON")"
  if command -v python3 >/dev/null 2>&1; then
    MCP_JSON_PATH="$MCP_JSON" MCP_SERVERS_JSON="$MCP_SERVERS" python3 - <<'PY'
import json, os
path = os.environ["MCP_JSON_PATH"]
new = json.loads(os.environ["MCP_SERVERS_JSON"])
data = {"mcpServers": {}}
if os.path.isfile(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except ValueError:
        backup = path + ".bak"
        os.replace(path, backup)
        print(f"existing mcp.json was not valid JSON; backed up to {backup}")
        data = {"mcpServers": {}}
data.setdefault("mcpServers", {}).update(new)
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(f"mcp.json updated: {path}")
PY
  elif [ ! -s "$MCP_JSON" ]; then
    printf '{\n  "mcpServers": %s\n}\n' "$MCP_SERVERS" > "$MCP_JSON"
    echo "mcp.json written: $MCP_JSON"
  else
    echo "note: python3 not found and $MCP_JSON is non-empty; merge this entry manually:" >&2
    echo "$MCP_SERVERS" >&2
  fi
fi

# ---- register into the personal plugin market (optional) -------------------------
DAIMON_BIN="${DAIMON_RUNTIME_BINARY_PATH:-}"
if [ "$DO_REGISTER" = 1 ]; then
  if [ -z "$DAIMON_BIN" ] && command -v kimi-daimon >/dev/null 2>&1; then
    DAIMON_BIN="$(command -v kimi-daimon)"
  fi
  if [ -n "$DAIMON_BIN" ] && [ -f "$DAIMON_BIN" ]; then
    SHARE_DIR="${KIMI_SHARE_DIR:-}"
    args=(kimi-plugin register-personal "$OUT_DIR" --json)
    [ -n "$SHARE_DIR" ] && args+=(--share-dir "$SHARE_DIR")
    if "$DAIMON_BIN" "${args[@]}"; then
      echo "registered into personal plugin market (install from the 'Personal' tab in Kimi desktop)"
    else
      echo "warning: kimi-daimon registration failed; register later or install by hand" >&2
    fi
  else
    echo "note: kimi-daimon not found; to register the plugin into the personal market run:"
    echo "  kimi-daimon kimi-plugin register-personal \"$OUT_DIR\" --json"
    echo "or use the Kimi desktop plugin page. The mcp.json entry above already enables the tools."
  fi
fi

# ---- verify -----------------------------------------------------------------------
IREZ_CLI="$CLI_BIN" IREZ_STATE_DIR="$STATE_DIR" "$MCP_BIN" doctor --stdio

echo
echo "Done. Next steps:"
echo "  1. Kimi desktop -> Plugins -> 'Personal' tab -> install 'IREZ IR Evidence' (if registered)."
echo "  2. Start a NEW Kimi session (mcp.json is read at session start)."
echo "  3. Ingest before querying: \"$CLI_BIN\" --state-dir \"$STATE_DIR\" ingest llvm <file.ll> --index full"
