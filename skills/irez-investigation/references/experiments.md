# IREZ Release Experiment

Use this workflow to test a newly installed IREZ release without reverting to manual
database browsing.

1. Call `irez_status`. Record `state_dir` (confirm it is the state you
   prepared), `irez_version`, DB/API/analysis/adapter versions, and
   `llvm_build_version`. Stop on an incompatible-state error; never modify the DB.
2. Call `irez_artifacts`; select artifacts from returned provenance rather than assumed
   ordering. If required artifacts are absent, report the exact CLI ingest commands and
   stop because MCP intentionally cannot ingest.
3. Call `irez_capabilities` and locate the target with `irez_functions` for each artifact.
4. Call bounded `irez_show` summaries on the functions. Do not request exact functions.
5. For return-value questions, call `irez_trace_return` with 50 nodes, depth 8, and
   `detail="summary"`; request the graph projection only when nodes or relations
   are material to the answer.
6. Inspect its chain, counts, direct calls, named call boundaries, evidence,
   unknowns, and truncation. Show only the small set of handles whose exact flags/text
   are material. Increase a budget only for a relevant returned boundary.
7. For a conditionally executed relevant call, call `irez_guards` on that callsite.
8. Compare only evidenced structural facts across artifacts. Do not claim semantic
   equivalence merely because names, counts, or return shapes match.
9. Measure tool-call counts from the captured MCP event log with
   `scripts/count_tool_calls.py` (never from agent self-report), and report whether
   any response had `encoding_lossy=true`. A lossy response is valid JSON, but
   replacement-bearing textual fields are not lossless.

The experiment passes the protocol/UX check when it completes through bounded MCP calls,
does not enumerate every block, never requires complete function IR, and reports every
capability, boundary, unknown, truncation, and lossy-text condition honestly.
