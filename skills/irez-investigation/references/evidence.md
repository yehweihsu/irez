# Interpreting IREZ Evidence

## Contents

1. Evidence hierarchy
2. Capability vocabulary
3. Envelope fields
4. Bounded claims
5. Common reasoning errors

## 1. Evidence hierarchy

Treat IREZ output in this order:

1. exact entity payload and exact structural relations;
2. exact source frames when present;
3. conservative or partial relations with explicit qualification;
4. unknown and unsupported capability declarations;
5. hypotheses derived from evidence, clearly labeled as hypotheses.

An Agent explanation is never stronger than the evidence and capability that support
it.

## 2. Capability vocabulary

`status` and `precision` are independent:

- `supported`: the capability is available in its declared scope;
- `partial`: only part of the capability is available;
- `unsupported`: no result is provided by this implementation;
- `must`: relation holds according to its producer;
- `may`: conservative candidate;
- `unknown`: producer cannot determine modality;
- `exact`: no intended approximation in the declared structural scope;
- `conservative`: may contain extra candidates;
- `partial`: omits portions outside the producer's supported scope;
- `heuristic`: evidence comes from a fallible rule.

Do not upgrade any term while paraphrasing.

## 3. Envelope fields

Read every response as:

- `result`: evidence returned by the command;
- `capabilities_used`: capabilities and precision used to produce it;
- `boundaries`: known next nodes excluded by a budget or scope;
- `unknowns`: facts the command could not determine;
- `truncation`: whether a node/depth budget stopped traversal;
- `evidence_refs`: artifact or analysis-run provenance;
- `expandable`: handles that can be queried next;
- `diagnostics`: operational notices.

`exit code 0` means the command succeeded, not that the result is complete.

## 4. Bounded claims

Good:

> The backward exact operand graph from instruction X visited 34 nodes within a
> 50-node budget and depth 8. It was not truncated. Control and memory dependencies
> were not included.

Bad:

> All causes of X were found.

Good:

> The callsite has an exact direct-call relation to an external declaration. IREZ
> cannot determine its effects.

Bad:

> The external call has no side effects.

Good:

> Immediate CFG predecessor evidence identifies predicate P for successor 1; the
> guard capability is partial/conservative.

Bad:

> P is the exact path condition for the target.

## 5. Common reasoning errors

- `truncated=false` does not cover unsupported relations.
- An absent source frame does not mean generated or unreachable code.
- Exact instruction text does not imply exact runtime behavior.
- Operand dependency is not memory dependency.
- CFG reachability is not a solved path condition.
- Direct use is not transitive influence.
- Function name similarity is not cross-artifact correspondence.
- A catalog body fingerprint is not semantic equivalence.
- An external declaration is not an effect summary.
- A renderer is not an algebraic simplifier.
