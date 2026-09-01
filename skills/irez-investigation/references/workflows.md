# IREZ Investigation Workflows

## Contents

1. Locate a target function
2. Locate an instruction
3. Explain a returned value
4. Find consumers of a value
5. Investigate a call boundary
6. Investigate source mapping
7. Investigate guards
8. Build bounded context
9. Stop conditions

## 1. Locate a target function

1. Call `irez_status`.
2. Call `irez_artifacts` when more than one artifact may be present; `irez_functions`
   defaults to the most recently ingested artifact, so choose explicitly.
3. Call `irez_capabilities` for the chosen artifact.
4. Call `irez_functions(artifact=..., match="<specific regex>")`.
5. If no match, broaden the regex once and report both searches.
6. If multiple matches remain, compare names, signatures, declaration status, and
   artifact provenance. Do not choose by ordinal alone.

## 2. Locate an instruction

1. Choose the child kind implied by the question: return, call, or block.
2. Call `irez_show(function_handle, view="children", kind=..., budget_nodes=N)`.
3. If truncated, continue only when a returned boundary is relevant.
4. Show only candidate instruction handles and match exact `opcode`, `llvm_type`,
   `attributes`, and `exact_text`, not only an SSA display name.

Do not enumerate every block to locate a return or callsite.

If the target is described by source location, materialize the function, inspect
candidate instructions with `irez_source`, and retain all ambiguous candidates.

## 3. Explain a returned value

1. Call `irez_trace_return(function_handle, return="all", budget_nodes=50,
   budget_depth=8)` — the default `detail="summary"` gives the chain spine,
   node and relation counts, `value_shape`, direct calls, named call
   boundaries, and per-site truncation at a fraction of the graph payload.
2. Inspect all return sites, capabilities, truncation, unknowns, and direct calls.
3. If multiple return sites exist and only one matters, repeat with that returned
   return-instruction handle.
4. Request `detail="graph"` (or `include=["nodes","relations"]`) only when the
   actual node list or relation edges are needed for the answer.
5. When fast-math or other boolean instruction flags are the question, add the
   `flags` section (`include=["chain","flags"]`) instead of showing individual
   instructions.
6. If a relevant operand is a boundary, expand selectively with a larger
   budget.
7. Show only final nodes whose precise text or non-boolean attributes matter.

State the bounded claim: “Within the exact operand graph, budget N, depth D...” Do not
rewrite the graph into a higher-level mathematical operation unless that relation is
separately evidenced.

## 4. Find consumers of a value

1. Use `irez_uses` for direct users of the known value handle.
2. Show a returned user only when its exact identity must be confirmed.
3. If the question is transitive, use a forward operand slice.
4. Preserve operand ordinals when explaining how a user consumes the value.
5. If truncated, report boundary user handles before increasing the budget.

## 5. Investigate a call boundary

1. Show the call instruction.
2. Inspect `llvm.calls` and its precision.
3. Classify the boundary from `status` plus `reason`:
   - direct internal (`reason` is `catalog_only`/`structure_ready`): expand if
     the callee matters;
   - direct external (`reason` is `declaration_only`): report effects unknown
     and stop at the boundary;
   - unknown (`reason` is `target_not_indexed`): report the dangling edge; do
     not attempt expansion;
   - indirect: report partial/unknown candidates and stop unless external evidence is
     available.
4. After internal expansion, investigate the callee as a new function-local scope.
5. Keep caller and callee evidence references distinct.

## 6. Investigate source mapping

1. Show the instruction first.
2. Call `irez_source`.
3. Report frames in `inline_depth` order.
4. If no frames exist, report source mapping unavailable/partial; continue structural
   investigation if it answers the question.
5. Do not infer source code semantics from filenames or scope names alone.

## 7. Investigate guards

1. Confirm the target instruction and containing block.
2. Call `irez_guards` with a small budget.
3. Report terminator, predicate, and required successor.
4. Label current V0 results partial/conservative.
5. Do not convert the result into a complete Boolean path condition.
6. If the question requires exact control dependence, stop and report that the
   required capability is unavailable.

## 8. Build bounded context

Use `irez_context` once a non-return target and investigation question are stable;
do not manually call show, source, and graph first merely to assemble the same packet.
Choose a budget that fits the known local graph. Verify:

- exact entity payload is present;
- source absence is explicit;
- value graph boundaries are visible;
- observation status is not mistaken for static evidence;
- evidence refs identify the artifact.

Use context as a handoff packet, not as proof of semantics beyond its declared
capabilities.

## 9. Stop conditions

Stop and report rather than speculate when:

- the required relation is unsupported;
- a result is unknown and there is no expandable boundary;
- an external/indirect call is essential but unresolved;
- exact control or memory dependence is required;
- the target is ambiguous after reasonable catalog/source filtering;
- expanding the budget would only make the response larger without answering the
  question;
- the investigation state or artifact is missing.
