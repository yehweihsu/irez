---
name: irez-investigation
description: Investigate LLVM IR artifacts and entities through the IREZ MCP tools using bounded graph queries, exact evidence, provenance, and capability-honest conclusions. Use when an Agent must locate LLVM functions or instructions, inspect operands or uses, trace value flow, examine source mappings or call boundaries, request guard evidence, build a context packet, or explain an IREZ result without reading an entire IR dump.
---

# IREZ Investigation

Use IREZ as an evidence index, not as an omniscient program verifier. Keep every query
bounded and distinguish exact structural evidence from partial, conservative, unknown,
and unsupported results.

## Core workflow

1. Call `irez_status`. Stop and report a configuration problem if the investigation
   is unavailable or empty.
2. Call `irez_capabilities` for the target artifact before relying on a relation.
3. Locate the function with `irez_functions`; pass its returned `handle` unchanged.
4. Choose the query from the question, not from database containment order:
   - returned value: `irez_trace_return` (start with `detail="summary"`; request
     the graph projection only when nodes/relations are actually needed);
   - direct/transitive consumers: `irez_uses` / a forward operand slice;
   - conditional call or instruction: locate the callsite, then `irez_guards`;
   - known value provenance: a small backward `irez_graph` or `irez_context`.
5. Use bounded `irez_show(view="children", kind=...)` only when a return, call, or
   block handle must be selected. Never enumerate every block by default.
6. Show an individual entity only when its exact opcode, flags, attributes, or source
   identity matter. Request `view="exact"` for a function only when complete IR is
   explicitly necessary.
7. Increase graph budgets only when a returned boundary is relevant.
8. For direct internal calls, use `irez_expand` only when callee evidence matters. Preserve
   external/indirect/effects-unknown boundaries.
9. Inspect `capabilities_used`, `unknowns`, `boundaries`, `truncation`,
   `evidence_refs`, and `expandable` before drawing a conclusion.
10. Report exact observations separately from limitations and hypotheses.

## Non-negotiable rules

- Never treat `partial`, `conservative`, `unknown`, or `unsupported` as `exact`.
- Never describe immediate CFG predecessor evidence as an exact path condition or
  complete control dependence.
- Never infer a runtime value from static SSA evidence.
- Never interpret missing debug information as evidence that no source relationship
  exists.
- Never claim a slice is complete merely because `truncated` is false; state the
  relations and capabilities included.
- Never construct an entity handle manually.
- Never request or reproduce the complete raw artifact unless the user explicitly
  needs it and bounded evidence cannot answer the question.
- Never use `show(function, view="exact")` or enumerate all blocks as a discovery step.
- Treat `encoding_lossy=true` as a successful but non-lossless protocol response:
  structural handles remain usable, but replacement-bearing text is not exact text.
- Do not silently increase budgets. State why expansion is necessary.

## Reference routing

Read only the references required for the task:

- Read [references/tools.md](references/tools.md) when choosing tools, parameters,
  relation filters, direction, or budgets.
- Read [references/workflows.md](references/workflows.md) for function discovery,
  value-flow, call-boundary, source, guard, or context investigation playbooks.
- Read [references/evidence.md](references/evidence.md) before interpreting an
  envelope, combining capabilities, or making a correctness claim.
- Read [references/reporting.md](references/reporting.md) when producing a user-facing
  investigation result, handoff, or evidence summary.
- Read [references/experiments.md](references/experiments.md) when validating a new
  IREZ release, comparing optimized/unoptimized artifacts, or measuring whether the
  bounded workflow avoids low-level containment browsing.

For a broad investigation, read `workflows.md` and `evidence.md`. For a single
well-scoped lookup, `tools.md` alone is usually enough.

## Query escalation

Use this order:

```text
catalog
  → question-specific composed/local query
  → bounded children lookup if a target handle is still needed
  → exact individual entity lookup
  → bounded operand graph or slice
  → wider/deeper slice
  → call expansion
  → context packet
```

Skip levels that do not serve the question. Stop when the evidence answers the
question or the next step requires an unsupported capability.

## Completion condition

Finish only when the response identifies:

- the target artifact and entity;
- the exact evidence observed;
- the relations and budgets used;
- all material unknowns, boundaries, and truncation;
- whether further expansion is possible;
- which statements are facts and which are hypotheses.
