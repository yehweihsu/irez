#!/usr/bin/env bash
# setup-mcp-host.sh -- one-shot irez-mcp host registration (Linux/WSL).
#
# Usage:
#   scripts/setup-mcp-host.sh [opencode|codex] [-- <extra irez-mcp install args>]
#
# Builds whatever is missing (C++ CLI via make, Rust server via cargo),
# registers the MCP server with the chosen host, links the
# irez-investigation skill, and verifies the chain with `doctor`.
# See docs/MCP_SETUP.md.
set -euo pipefail

HOST="${1:-opencode}"
if [[ $# -gt 0 ]]; then shift; fi
case "$HOST" in
  opencode|codex) ;;
  *) echo "usage: $0 [opencode|codex]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${IREZ_BUILD_DIR:-$ROOT/build}"
CLI="$BUILD_DIR/irez"
MCP="$ROOT/mcp/target/release/irez-mcp"

if [[ ! -x "$CLI" ]]; then
  echo "== CLI not built; running make build (BUILD_DIR=$BUILD_DIR) =="
  make -C "$ROOT" build BUILD_DIR="$BUILD_DIR"
fi
if [[ ! -x "$MCP" ]]; then
  echo "== irez-mcp not built; running make mcp =="
  make -C "$ROOT" mcp
fi

echo "== Registering irez-mcp with $HOST =="
"$MCP" install "$HOST" --cli "$CLI" "$@"

echo "== Verifying =="
"$MCP" doctor "$HOST" --cli "$CLI"

echo
echo "Done. State dir: ~/.local/share/irez/$HOST (override with --state-dir)."
echo "Prepare it with the CLI before querying, e.g.:"
echo "  $CLI --state-dir ~/.local/share/irez/$HOST ingest llvm $ROOT/fixtures/nonfloating.ll --index full"
