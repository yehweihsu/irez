# IREZ MCP Tool Selection

## Contents

1. Global selection rules
2. Catalog and capability tools
3. Entity tools
4. Traversal tools
5. Control and call tools
6. Context tool
7. Budget selection

## 1. Global selection rules

Choose tools from the investigation question. Pass the canonical `handle` exactly as
returned (`id` and `entity_id` are temporary compatibility aliases). For every
graph-like result, inspect the entire outer envelope, not only `result`.

MCP does not expose investigation creation or artifact ingest. If `irez_status` shows
no usable state, ask the user to initialize and ingest through the CLI.

## 2. Catalog and capability tools

### `irez_status()`

Use first. Confirm the resolved `state_dir` (absolute, forward slashes),
schema, artifact count, function count, and materialization count. When
artifact or function counts look wrong, check `state_dir` before anything
else — a mismatched state directory is the most common cause and is otherwise
silent.

### `irez_artifacts()`

Use to list ingested artifacts and their handles. `irez_functions` without an
`artifact` argument reads only the most recently ingested artifact, so when the
investigation holds more than one artifact, pick the right one here first and
pass its id explicitly afterwards.

### `irez_capabilities(artifact?)`

Use before relying on CFG, source, direct calls, or another relation. Omit `artifact`
only when the investigation contains one obvious current artifact. Record both
`status` and `precision`.

### `irez_functions(artifact?, match?)`

Use to discover function handles. `match` is a regex over function names. Start
specific, then broaden. Do not invent compiler name mangling rules without evidence.

Function `status` vocabulary: `catalog_only` (body not yet loaded),
`structure_ready` (structure loaded), `declaration_only` (external declaration;
there is no body to load, and materializing it will never produce structure).

## 3. Entity tools

### `irez_show(handle, view="summary", kind?, budget_nodes=100)`

Function `summary` is bounded: signature, materialization, counts, source summary,
capabilities, and provenance. Use `view="children"` plus `kind="return"`, `"call"`,
or `"block"` to locate only the required children. Use `view="exact"` for complete
function IR only when the exact full body is explicitly required. Individual
instructions remain suitable for exact inspection.

### `irez_source(handle)`

Use for debug/source and inline frames. An empty result plus an unavailable unknown is
a valid partial response.

### `irez_uses(handle, budget_nodes=100)`

Use for direct operand users. This is a local indexed query, not a transitive forward
slice. Lower the budget for exploratory use; increase only if truncation returns
relevant boundaries.

## 4. Traversal tools

### `irez_slice(...)`

Parameters:

- `handle`: exact target handle.
- `direction`: `backward` for instruction-to-operands; `forward` for users.
- `relations`: comma-separated aliases such as `operand`, `call`, `cfg`, `control`.
- `budget_nodes`: maximum visited nodes.
- `budget_depth`: maximum relation depth.
- `call_depth`: call expansion budget; V0 does not imply automatic full call-graph
  traversal.

Use `relations="operand"` for pure value flow. Adding an unsupported relation should
produce an unknown rather than silently changing semantics.

### `irez_graph(handle, direction="backward", budget_nodes=100, format="json")`

Use for the operand/value projection. Select `format="json"` for machine reasoning and
`format="exact-ir"` when the exact LLVM text is material to the answer. The renderer
does not perform algebraic simplification.

## 5. Control and call tools

### `irez_guards(handle, budget_nodes=100)`

Reports the guards of the instruction's containing block. Artifacts ingested
with control-dependence support (adapter v1 with `control_dependence`
capability) yield exact answers: the branches the block is control-dependent
on, with the required successor edge and the branch predicate. A block that
post-dominates its region correctly reports no guards. Older state
directories without control-dependence evidence fall back to conservative
immediate-CFG-predecessor evidence, flagged `partial/conservative`; that is
not exact control dependence or a solved path condition. Neither mode claims
runtime path conditions.

### `irez_expand(handle)`

On a function, request materialization. On a direct internal callsite, request callee
materialization. Preserve these terminal boundaries:

- external: effects unknown, not expandable;
- indirect: partial or unknown;
- missing call relation: unknown.

Do not repeatedly expand an external or unknown boundary.

## 6. Context tool

### `irez_trace_return(handle, return="all", budget_nodes=50, budget_depth=8, detail="summary", include=[])`

Preferred first evidence query for “how is this function's returned value produced?”
Pass a function handle. It composes return sites, operands, bounded function-local
backward value graphs, direct calls, source evidence, capabilities, unknowns, and
truncation. It does not perform semantic interpretation or cross-function expansion.
Select one returned return-instruction handle only when multiple sites must be separated.

Projection controls:

- `detail="summary"` (the default) replaces the graph payload with a compact
  `chain` spine plus `node_count`, `relation_count`, and `value_shape`
  (`scalar`/`vector`/`void`) per return site, keeping source evidence, direct
  calls, call boundaries, and per-site truncation.
- `detail="graph"` returns the full `value_graph` nodes/relations; request it
  only when the actual node list or relation edges are needed for the answer.
- `include=[...]` adds sections on top of the detail default; legal sections
  are `chain`, `calls`, `source`, `nodes`, `relations`, `boundaries`, and
  `flags`. Unknown sections are usage errors.
- The `flags` section is opt-in only: it sparsely lists graph nodes carrying
  non-default boolean instruction attributes (nsw, exact, and the fast-math
  family) as `{handle, opcode, flags}` rows. A requested section with no
  flagged nodes returns an empty array, so “checked, none set” is
  distinguishable from “not requested”. Use it instead of per-instruction
  `irez_show` calls when flags are the question.

`call_boundaries` entries carry `target_name` and `target_llvm_type` for
direct calls, so a summary call usually answers “which callee?” without a
follow-up `irez_show`. Each boundary also carries a `reason`: the target's
materialization state (`declaration_only` means genuinely external —
`status="external"`, not expandable; `catalog_only`/`structure_ready` mean an
internal definition, expandable), or `target_not_indexed` for a dangling call
edge (`status="unknown"`, never expandable). Graph nodes remain intentionally
compact; call `irez_show`
only for final handles whose exact instruction text or non-boolean attributes
matter. Never repeat an identical trace-return call to re-extract a field —
select sections with `include` instead. Per-site `truncation` is emitted in
both modes and is part of the correctness contract.

When every return is `void` or a constant null pointer and the function
contains stores (the XLA/Numba/Triton kernel shape), `irez_trace_return`
flags this in `unknowns` (`kind: result_channel`) — switch to
`irez_trace_stores` for the actual result channel instead of drilling through
`irez_context`/`irez_graph` by hand.

### `irez_trace_stores(handle, budget_nodes=50, budget_depth=8, detail="summary", include=[])`

The store-sink twin of `irez_trace_return`: for every `store` instruction in
the function it traces the stored value backward with the same bounded
function-local operand evidence and the same projection contract
(`detail`/`include`). Each site reports the `store` instruction, the stored
`value` (with `value_shape`), and the destination `pointer`. A function with
no stores returns `store_count: 0` plus a `store_sites: none` unknown — an
honest empty result, not an error.

### `irez_context(handle, budget_nodes=100)`

Use after a non-return target is known to package entity, source, value graph,
capabilities, unknowns, and evidence references. A known value-provenance question may
use it directly; do not manually reproduce its component calls first.

Context is ephemeral. Runtime observation is unavailable unless explicitly supplied
by a separate evidence source.

## 7. Budget selection

Suggested starting budgets:

| Question | Nodes | Depth |
|---|---:|---:|
| Direct uses | 20 | n/a |
| Local instruction operands | 20 | 4 |
| Return-value explanation | 50 | 8 |
| Wider function-local slice | 100 | 12 |

Increase a budget only when:

1. `truncated` is true;
2. a returned boundary is relevant to the question;
3. the relevant relation is supported at an acceptable precision.

Prefer targeted expansion from a boundary over doubling every budget.
