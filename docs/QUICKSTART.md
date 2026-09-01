# Quick start

Download and extract the `linux-x86_64` or `windows-x86_64` release. Paths
below are relative to the extracted directory (`.exe` is implied on Windows).

For Linux/WSL, extract the tarball from a Linux shell so its executable modes
are preserved. To extract directly into a chosen directory:

```bash
mkdir -p Linux_Install
tar -xzf irez-X.Y.Z-linux-x86_64.tar.gz -C Linux_Install --strip-components=1
cd Linux_Install
```

Do not use a Windows archive GUI to write directly into the WSL filesystem.
If that already happened and `bin/irez` reports `Permission denied`, restore
the modes recorded in the tarball before continuing:

```bash
chmod 755 bin/irez bin/irez-llvm-index bin/irez-mcp
```

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
./bin/irez --state-dir /absolute/path/to/state ingest llvm example.ll --index full
```

Starting from C/C++ sources rather than a ready `.ll`, or switching the host
between projects, is covered in [NEW_PROJECT.md](NEW_PROJECT.md).

For another MCP host, follow [MCP_OTHER_HOSTS.md](MCP_OTHER_HOSTS.md).

To validate a new release in a fresh Agent window with bounded `show` and
`trace-return`, follow [EXPERIMENT.md](EXPERIMENT.md).

If something does not work — `doctor` reporting a failure, `database is
locked`, a rejected state schema — start at
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
