# Open Questions — IREZ V00_01

Unresolved design choices. Record them here instead of growing an abstraction
to avoid a local decision. Newest first.

## 2026-08-04 — from the fld1 rerun review

These came out of the experiment agent's round-trip analysis. Each was
evaluated against the "no redundant layering" rule; none is a correctness
bug, so none was fixed in the hardening pass.

1. **Artifact deletion / GC.** No command removes a stale artifact from a
   state directory; regenerated artifacts with identical content hash to
   nothing, and obsolete ones accumulate, raising pick-the-wrong-artifact
   risk. Open: is this a CLI `delete artifact` (new surface), or a documented
   "create a fresh state" workflow? Deferred — the prototype's answer is a
   fresh state directory, but that answer erodes as states grow.
2. **Truncation resume cursor.** When a slice/trace hits `depth_budget` or
   `node_budget`, the only continuation is rerunning with a larger budget and
   retransmitting the whole payload. A `resume_from=<boundary handle>` cursor
   would avoid that. Open: cursor semantics vs. simply making boundary
   handles cheap to re-query individually. Deferred until a second workload
   demonstrates the need.
3. **Summary chain topology.** The `chain` spine mixes instructions,
   function, argument, and constant nodes in traversal order; precise
   operand-position topology ("fdiv operand 0 is fsub") still needs targeted
   `show`. Open: ordinal annotations on chain entries vs. accepting the
   follow-up call. Deferred — the `flags` section showed opt-in sections can
   close such gaps without default payload growth if this one proves hot.
4. **`encoding_lossy` implicit false.** The field is absent (not explicitly
   `false`) on lossless responses, so consumers cannot distinguish "lossless"
   from "field not implemented". Making it explicit changes every envelope
   and every golden. Deferred — acceptable while the field is documented as
   optional, but revisit before a 1.0 contract freeze.
