# IREZ CLI guide

`irez` is the single entry point for state preparation and evidence queries.
Every command prints one JSON envelope on stdout; the envelope schema,
handle format, and exit codes are stable contract.

Global options must precede the command:

```text
irez [--state-dir DIR] COMMAND ...
```

The state directory defaults to `IREZ_STATE_DIR`, or `.irez` when unset.

## Commands

### State preparation

| Command | Purpose |
|---|---|
| `init --name NAME` | create a state directory and investigation |
| `ingest llvm PATH [--index catalog\|full] [--refresh]` | store content once, refresh failed/stale catalogs when needed, and satisfy the requested index level; the response echoes the resolved absolute `state_dir` so a CLI/MCP default mismatch is visible immediately |
| `reindex ARTIFACT [--index catalog\|full]` | rerun catalog and analysis from the immutable stored artifact with the current adapter |

### Query commands

| Command | Purpose |
|---|---|
| `status` | investigation, resolved `state_dir`, artifact/function/materialization counts, plus diagnostics: a `state_dir_conflict` warning when another well-known state location holds a database, and a `materialization_counts` note defining the count semantics (storage-level `structure_ready` rows; query-triggered materialization is included once the query completes — see `functions` for per-function state) |
| `artifacts` | ingested artifacts with ids, paths, and content hashes |
| `capabilities [--artifact ID]` | adapter capabilities with status/precision per feature |
| `functions [--artifact ID] [--match REGEX]` | cataloged functions; declarations report `declaration_only` |
| `materialize function HANDLE` | parse one function's full structure (entities, relations, source map) into the store |
| `show HANDLE [--view summary\|exact\|children] [--kind block\|return\|call] [--budget-nodes N]` | bounded function summary by default; explicit complete IR or bounded child navigation |
| `source HANDLE` | source-location chain (file/line/scope/inline depth) |
| `uses HANDLE [--budget-nodes N]` | entities that use the target |
| `slice HANDLE [--direction ...] [--relations LIST] [--budget-* N]` | bounded graph walk over selected relation kinds (`operand`, `control`, `call`, `cfg`; default `operand,control`). Traversal is function-scoped: slicing a module-scoped entity (e.g. a global) returns the target node alone and reports a `scope` unknown instead of an unexplained empty result |
| `graph HANDLE [--view value] [--direction ...] [--format json\|exact-ir]` | operand/value projection; `exact-ir` is LLVM-normalized text, not an original-file byte slice |
| `guards HANDLE [--budget-nodes N]` | guard evidence for the target's block: exact control dependence on CD-capable artifacts, conservative CFG predecessors otherwise |
| `expand HANDLE` | materialize a function, or the callee of a direct internal callsite |
| `context HANDLE [--budget-nodes N]` | investigation packet: entity + source + value graph + explicit unknowns |
| `trace-return FUNCTION [--return all\|HANDLE] [--budget-nodes N] [--budget-depth N] [--detail summary\|graph] [--include LIST]` | composed, bounded function-local return/value/source evidence |
| `trace-stores FUNCTION [--budget-nodes N] [--budget-depth N] [--detail summary\|graph] [--include LIST]` | the store-sink twin: traces each store's stored value backward — the result channel of kernel-shaped functions (`ret void` / `ret ptr null`) |

`trace-return` keeps relations and provenance but uses compact graph nodes. Query
individual returned handles with `show` when exact instruction text or
non-boolean attributes are material to the conclusion.

Projection: `--detail summary` (the default) replaces the per-site `value_graph`
with a compact `chain` spine plus `node_count`, `relation_count`, and
`value_shape` (`scalar`/`vector`/`void`), while keeping source evidence, direct
calls, call boundaries, and per-site truncation. `--detail graph` returns the
full nodes/relations. `--include` (comma-separated) adds sections on top of the
detail default (for example `--include flags` under `--detail graph` keeps
nodes/relations and adds the flags rows); legal sections are `chain`, `calls`,
`source`, `nodes`, `relations`, `boundaries`, and `flags`, and unknown sections
are usage errors (exit 2). The opt-in `flags` section sparsely lists nodes
carrying non-default boolean instruction attributes (nsw, exact, fast-math
family) as `{handle, opcode, flags}` rows; a requested section with no flagged
nodes returns an empty array.

`trace-stores` shares this projection contract; per site it reports the `store`
instruction, the stored `value` (with `value_shape`), and the destination
`pointer`. When every return of a function is `void` or a constant null pointer
and the function contains stores, `trace-return` flags the shape in `unknowns`
(`kind: result_channel`) and points at `trace-stores`. `call_boundaries` entries include `target_name` and
`target_llvm_type` for direct calls, plus a `reason` field: the target's
materialization state (`declaration_only` = genuinely external;
`catalog_only`/`structure_ready` = internal and expandable) or
`target_not_indexed` (`status="unknown"`, never expandable). Per-site
`truncation` is emitted in both modes.

## Handles

Handles are content-addressed:

```text
irez:<artifact-hash-16>:llvm:<kind>:<function-key>[:<ordinal>]
```

e.g. `irez:5b72ac864e894652:llvm:inst:f1:7`. Function keys (`f0`, `f1`, ...)
are module ordinals, so handles are deterministic for identical artifact
content.

## Envelope

Every response carries `schema_version` (the V0 alias), `api_schema_version`,
`command`, `investigation`, `target`, `result`,
`capabilities_used`, `unknowns`, `boundaries`, `expandable`,
`evidence_refs`, and `truncation`. Honesty rules:

- capabilities are reported per command with `status`
  (`supported`/`partial`/`unsupported`) and `precision`
  (`exact`/`conservative`);
- anything the store cannot prove appears in `unknowns` with a reason,
  never as silence;
- budget cut-offs are reported in `truncation`; reaching an already-known
  node at maximum depth is not truncation.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success (including honestly declared partial results) |
| 2 | usage error (the stderr JSON carries a `usage` field with the correct syntax for the command being parsed) |
| 3 | target not found |
| 4 | adapter/parser failure (the IR could not be parsed) |
| 5 | state/database/local IO/internal failure |
| 6 | unsupported operation or artifact type |
| 7 | another process owns a live catalog/materialization claim |

Exit code 1 is never used: every failure maps to a specific code above, and
the stderr JSON always carries `error`, `error_kind`, and `exit_code`.

## State directories and versioning

A state directory is portable across machines and OSes only while its
`db_schema_version` is supported. The prototype release does not modify an
existing old or future schema: it rejects it and asks the user to create a new
state and reindex the original artifacts. Missing or stale analyzer capability
manifests inside the supported DB schema trigger refresh from the immutable
artifact; `reindex` forces this explicitly.

## Standalone adapter

`irez-llvm-index` (`catalog`, `function <key>`, `version` subcommands) emits
the adapter's JSONL record stream directly; it exists for debugging and
adapter cross-checks and is not needed for normal CLI or MCP use.
