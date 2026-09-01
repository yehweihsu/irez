# Connecting IREZ to an agent host (MCP)

`irez-mcp` is a thin MCP server (stdio transport): it holds no state and
contains no query logic. Each tool call spawns the `irez` CLI and returns its
JSON envelope, so the CLI stdout contract is the IPC boundary. The server
exposes 14 query tools (`irez_status`, `irez_artifacts`, `irez_functions`,
`irez_show`, `irez_source`, `irez_uses`, `irez_slice`, `irez_graph`,
`irez_guards`, `irez_expand`, `irez_context`, `irez_capabilities`,
`irez_trace_return`, `irez_trace_stores`).

By design, MCP offers **no** `init` or `ingest` tool: prepare the state
directory with the CLI first, then investigate through the host.

Supported hosts: **OpenCode** and **Codex** (via the installer below), and
**Kimi Work** via `scripts/setup-kimi-plugin.sh` / `.ps1` (see
[MCP_OTHER_HOSTS.md](MCP_OTHER_HOSTS.md)).

## Binary release (recommended)

Extract the native Windows/Linux x86-64 bundle, then run:

```bash
./bin/irez-mcp install codex       # or opencode
./bin/irez-mcp doctor codex
```

The sibling CLI is discovered automatically. Both binaries are copied into a
versioned per-user directory before absolute paths are registered. The host
itself must be installed; Codex also needs its CLI on PATH.

## Source-tree wrappers

Linux / WSL:

```bash
scripts/setup-mcp-host.sh              # default: opencode
scripts/setup-mcp-host.sh codex
```

Windows (PowerShell):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-mcp-host.ps1           # default: opencode
powershell -ExecutionPolicy Bypass -File scripts\setup-mcp-host.ps1 -Agent codex
```

These thin developer wrappers build anything missing, then delegate all
configuration to the Rust installer. They register the server with the
host, install the embedded `irez-investigation` skill, and finish by running
`irez-mcp doctor` so a broken setup fails loudly. Extra arguments after the
host name are passed through to `irez-mcp install` (e.g. `--state-dir`).

## What the installer does

`irez-mcp install <opencode|codex> [--cli <path-to-irez>]` performs:

1. **Versioned binary install** — copies the CLI and MCP server into a stable,
   per-user `irez/versions/<version>/bin` directory.
2. **State init** — creates a per-host state directory
   (default `$XDG_DATA_HOME/irez/<host>` or `~/.local/share/irez/<host>`;
   on Windows `%LOCALAPPDATA%\irez\<host>`; override with `--state-dir`).
3. **Host registration** —
   - OpenCode: edits `~/.config/opencode/opencode.jsonc`
     (on Windows: `%USERPROFILE%\.config\opencode\opencode.jsonc`;
     override with `--opencode-config`) with a real JSONC scanner, after
     writing a timestamped backup next to the file.
   - Codex: shells out to `codex mcp add`.
   The registered command points at `irez-mcp` and sets `IREZ_CLI` and
   `IREZ_STATE_DIR` in its environment.
4. **Owned skill installation** — installs the recursively embedded skill
   with an ownership marker. Reinstall updates owned files; unowned paths are
   never overwritten or removed.

`irez-mcp uninstall <host>` removes only what the installer created; state
and ingested artifacts are kept. `irez-mcp doctor <host> --cli <path>` verifies
the CLI, state, owned host registration, a real MCP initialize, `tools/list`,
a sample `irez_status` call, and the owned skill installation.

To upgrade, extract the new bundle and run `irez-mcp update <host>`, then
`doctor`. Update makes a consistent SQLite backup before its schema
compatibility check and updates only installer-owned registration and skill files. See
[UPGRADING.md](UPGRADING.md).

## Preparing the state directory

The MCP tools only query; ingest with the CLI first. Example:

Linux / WSL:

```bash
STATE=~/.local/share/irez/opencode
build/irez --state-dir "$STATE" ingest llvm fixtures/nonfloating.ll --index full
```

Windows (cmd):

```bat
set STATE=%USERPROFILE%\.local\share\irez\opencode
build\irez.exe --state-dir "%STATE%" ingest llvm fixtures\nonfloating.ll --index full
```

## Verifying from the host

Restart the host after registration, then ask it to drive the tools, e.g.:

> Use the irez MCP tools: run `irez_status` and `irez_artifacts`, find the
> `choose` function with `irez_functions`, materialize it, then use
> `irez_trace_return` on it and describe the bounded return-value and
> control-flow structure (in particular the two predecessors of the phi
> node).

A healthy setup answers with the diamond CFG of `choose` in
`fixtures/nonfloating.ll` (entry → yes/no → merge with a phi). Fresh state
directories carry exact control-dependence evidence: `irez_guards` on `%a`
reports entry's `br i1 %cond` with required successor 0 (supported/exact).
An unsupported old or future DB schema is rejected without modification;
create a new state and reindex the original artifacts. Stale analysis inside
the supported schema is refreshed on demand from its immutable artifact.

After `doctor` succeeds, use the bounded new-window workflow in
[EXPERIMENT.md](EXPERIMENT.md) to exercise summaries, `trace-return`, guards,
capability reporting, truncation, and UTF-8 loss markers without dumping whole
functions.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| host shows no irez tools | restart the host; verify with `irez-mcp doctor <host> --cli <path>` |
| `refusing to replace unowned skill path` | move the existing path or explicitly reconcile ownership before reinstalling |
| tools return `state not found`/`no artifacts` | the state dir was never prepared; ingest with the CLI (above) |
| OpenCode config not found | pass `--opencode-config <path>`; default is `~/.config/opencode/opencode.jsonc` (`%USERPROFILE%\.config\opencode\opencode.jsonc` on Windows) |
| Windows: `irez-mcp.exe` fails to start | build with `.\scripts\build-mcp.ps1` on the MSVC toolchain (`x86_64-pc-windows-msvc`) |
