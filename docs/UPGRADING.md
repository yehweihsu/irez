# Updating IREZ

Extract the new release and run:

```bash
./bin/irez-mcp update codex
./bin/irez-mcp doctor codex
```

Use `opencode` for that host. Update copies the new binaries into a new
versioned directory, creates a transactionally consistent SQLite backup in
the state's `backups/` directory, checks the state schema, rewrites only
installer-owned registration, and replaces only an installer-owned skill.
Old version directories and state backups are retained for manual rollback.

The prototype supports only its current DB schema. An existing old or future
schema is identified and rejected without modification; create a new state
with the new binary and reindex the original artifacts. Automatic migration
is deliberately deferred until more than one released schema exists.

Compatibility is split into independently versioned surfaces:

- `db_schema_version`: persistent SQLite layout;
- `api_schema_version`: CLI/MCP JSON envelope and command semantics;
- `analysis_schema_version`: entity/relation/capability semantics;
- `adapter_version`: analyzer implementation contract;
- `llvm_build_version`: LLVM parser/analyzer build used;
- `irez_version`: product version;
- `registration_schema_version`: installer ownership/config record;
- `skill_version`: embedded Agent guidance.

The runtime values are exposed by `irez status` and recorded in release
metadata. A binary refuses any unsupported old or future state schema without
changing it. JSON readers ignore unknown optional fields; breaking changes
require an API-major increment rather than being silently accepted. The
installer never adopts or removes an unowned host entry or skill unless the
user explicitly uses `--force`.

The installer also records `cli_contract_version` and
`mcp_contract_version` for CLI/server pairing checks. Adding or removing a
command or MCP tool bumps the corresponding contract version even when the
individual JSON envelopes remain field-additive. This lets `doctor` diagnose
a mismatched CLI/server pair before it fails opaquely with `unknown command`.

For rollback, re-run `install --force` from the retained older bundle/version
and restore the matching backup only after stopping MCP host processes. Do
not open a state database with a binary whose documented schema differs.
