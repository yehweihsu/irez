# Troubleshooting

Run `irez-mcp doctor codex` or `irez-mcp doctor opencode` first. For an
unrecognized host, set `IREZ_CLI` and `IREZ_STATE_DIR`, then run
`irez-mcp doctor --stdio`. The layered output distinguishes executable,
state, registration, MCP initialization, tool listing, sample call, and skill
failures.

`database is locked` means another process is writing after the bounded busy
timeout. Retry after the analysis finishes. `analysis_in_progress` is a
structured status, not database corruption. Unsupported old and future schema
versions are both rejected without modifying the state: use the release that
created the state, or create/reindex a fresh state with the current release.
IREZ intentionally does not perform an implicit complex migration.

`encoding_lossy: true` means the response is valid UTF-8 JSON, but one or more
source strings contained bytes that could not be represented losslessly.
Entity handles and structural fields remain usable; text containing replacement
characters must not be cited as exact byte evidence.
