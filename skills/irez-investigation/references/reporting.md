# Reporting an IREZ Investigation

## Contents

1. Short answer format
2. Full evidence report
3. Language rules
4. Handoff checklist

## 1. Short answer format

Use:

```text
Finding:
<one bounded conclusion>

Evidence:
- <entity/relationship and exact observation>
- <source or call evidence if relevant>

Limits:
- <capability, unknown, boundary, or truncation>

Next expansion:
<specific handle/query, or "none">
```

## 2. Full evidence report

Include:

```text
Question
Target artifact and entity
Materialization state
Queries and budgets
Exact structural observations
Partial/conservative observations
Unknown or unsupported requirements
Truncation and boundary handles
Provenance/evidence references
Conclusion
Hypotheses
Recommended next query
```

Keep hypotheses separate from the conclusion.

## 3. Language rules

Prefer:

- “The exact operand relation shows...”
- “Within a 50-node backward slice...”
- “Source mapping is unavailable for this entity.”
- “The call target is external; effects are unknown.”
- “The guard result is partial/conservative.”

Avoid:

- “IREZ proved the whole function...”
- “No source exists.”
- “This value definitely occurs at runtime.”
- “The path condition is...”
- “The external call is harmless.”

## 4. Handoff checklist

Before delivering:

- Include handles exactly as returned.
- Include artifact/run evidence references.
- State direction, relation set, node budget, and depth budget.
- State whether truncation occurred.
- List material unknowns and unsupported capabilities.
- Identify expandable boundaries.
- Do not paste a full IR dump when a small exact excerpt is sufficient.
