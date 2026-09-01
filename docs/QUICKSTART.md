# Quick start

Download and extract the `linux-x86_64` or `windows-x86_64` release. Paths
below are relative to the extracted directory (`.exe` is implied on Windows).

## Codex

```bash
./bin/irez-mcp install codex
./bin/irez-mcp doctor codex
```

## OpenCode

```bash
./bin/irez-mcp install opencode
./bin/irez-mcp doctor opencode
```

The installer copies `irez` and `irez-mcp` into a versioned per-user install
directory, initializes a per-host state, registers absolute native paths, and
installs the bundled investigation skill. Use `--no-skill` to omit the skill.

Ingest evidence before asking the Agent to investigate it:

```bash
irez --state-dir /absolute/path/to/state ingest llvm example.ll --index full
```

Starting from C/C++ sources rather than a ready `.ll`, or switching the host
between projects, is covered in [NEW_PROJECT.md](NEW_PROJECT.md).

For another MCP host, follow [MCP_OTHER_HOSTS.md](MCP_OTHER_HOSTS.md).

To validate a new release in a fresh Agent window with bounded `show` and
`trace-return`, follow [EXPERIMENT.md](EXPERIMENT.md).

If something does not work — `doctor` reporting a failure, `database is
locked`, a rejected state schema — start at
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
