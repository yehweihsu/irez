# Testing a new IREZ release

This experiment checks the bounded Agent workflow and the CLI-to-MCP protocol boundary.
It assumes two related LLVM artifacts, such as unoptimized and optimized IR.

## Prepare evidence

MCP intentionally cannot initialize or ingest. Prepare a fresh state with the release
CLI, then register that same absolute state path with the host:

```bash
irez --state-dir /absolute/path/to/experiment-state init --name release-test
irez --state-dir /absolute/path/to/experiment-state ingest llvm before.ll --index full
irez --state-dir /absolute/path/to/experiment-state ingest llvm after.ll --index full
irez-mcp install codex --state-dir /absolute/path/to/experiment-state --force
irez-mcp doctor codex
```

Use `opencode` in the final two commands for that host. On Windows, use native absolute
Windows paths and `.exe` binaries; do not mix a Windows host with WSL executables.
Restart the host after registration.

## New-window prompt

Replace `TARGET_REGEX` and the artifact descriptions, then paste this as one message:

```text
Use $irez-investigation and the configured IREZ MCP to run a bounded release experiment.

Target function regex: TARGET_REGEX
Artifact A: describe the expected first artifact
Artifact B: describe the expected second artifact

First call irez_status and report irez_version, db_schema_version,
api_schema_version, analysis_schema_version, adapter_version, and
llvm_build_version. Then discover artifacts and functions from returned handles; do not
guess handles or artifact ordering.

For each selected artifact, inspect capabilities, call bounded function show summary,
and run irez_trace_return with budget_nodes=50 and budget_depth=8. Do not request a
complete function view and do not enumerate every block. Inspect compact nodes,
relations, direct calls, call boundaries, source evidence, capabilities, unknowns,
truncation, and evidence_refs. Show only individual handles whose exact text or flags
are necessary. Call guards only on relevant callsites returned by the bounded trace.

Compare only evidenced structural facts. Do not claim runtime behavior or semantic
equivalence from structural similarity. If a relevant boundary is truncated, explain
why it matters before making one targeted expansion. If artifacts are missing or the DB
schema is incompatible, stop and report the exact preparation command; do not modify or
migrate the state.

Finish with: versions, selected artifacts/functions, bounded findings, structural
differences, capabilities/unknowns, truncation/boundaries, evidence refs, every
encoding_lossy occurrence, and whether the new bounded workflow avoided
full-function IR and block-by-block browsing.
```

`encoding_lossy=true` means the response remained valid JSON but at least one textual
field contains replacement text. Handles and structural relations remain usable; do not
quote replacement-bearing text as byte-exact evidence.

## Measure tool-call counts

The MCP tool-call count is an evaluation metric owned by the harness, never
self-reported by the evaluated agent (agents miscount; the fld1 rerun reported
18 calls when the log held 20). After the session, export the host's event log
(JSON or JSONL) and run:

```bash
python scripts/count_tool_calls.py <session-export.jsonl>
```

The script reports total and per-tool calls and lists exact-duplicate
(tool, arguments) groups — identical repeated calls are the redundancy metric
of the bounded workflow, and they are what `irez_trace_return`'s
`detail`/`include` projection is meant to eliminate.
