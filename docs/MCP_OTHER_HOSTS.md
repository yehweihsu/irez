# Connecting IREZ to other MCP hosts

IREZ's stable public integration is a local MCP server over stdio.

```text
Transport: stdio
Command: /absolute/native/path/to/irez-mcp[.exe]
Arguments: none
Environment:
  IREZ_CLI=/absolute/native/path/to/irez[.exe]
  IREZ_STATE_DIR=/absolute/path/to/irez-state
```

A generic configuration is:

```json
{
  "name": "irez",
  "transport": "stdio",
  "command": "/absolute/path/to/irez-mcp",
  "args": [],
  "env": {
    "IREZ_CLI": "/absolute/path/to/irez",
    "IREZ_STATE_DIR": "/absolute/path/to/state"
  }
}
```

Use absolute paths. A Windows host must use Windows `.exe` files; a Linux or
WSL host must use Linux files. Do not point a Windows process at a WSL
executable. The server reserves stdout for JSON-RPC and sends logs to stderr.

Prepare the state with the CLI (MCP deliberately cannot init or ingest):

```bash
/absolute/path/to/irez --state-dir /absolute/path/to/state init --name default
/absolute/path/to/irez --state-dir /absolute/path/to/state \
  ingest llvm example.ll --index full
```

Verify IREZ independently of the host:

```bash
IREZ_CLI=/absolute/path/to/irez \
IREZ_STATE_DIR=/absolute/path/to/state \
/absolute/path/to/irez-mcp doctor --stdio
```

The doctor performs `initialize`, `tools/list`, and an `irez_status` call. A
healthy server is named `irez-mcp` and currently exposes 14 tools. Starting
the server without `doctor` appears to hang because it is correctly waiting
for JSON-RPC on stdin.

## Text to give another Agent

```text
Configure a local stdio MCP server named "irez".

Command:
  /absolute/path/to/irez-mcp

Environment:
  IREZ_CLI=/absolute/path/to/irez
  IREZ_STATE_DIR=/absolute/path/to/state

Do not add shell wrappers unless the host requires one. Use absolute native
paths for the operating system where the host runs. After configuration,
verify that the server exposes irez_status and the remaining IREZ tools.
```

## Kimi Work (Kimi desktop)

The supported path is the generator script, which works from a source tree or
an extracted release bundle and needs no hand editing:

```bash
scripts/setup-kimi-plugin.sh                 # Linux/WSL/macOS/Git Bash
powershell -ExecutionPolicy Bypass -File scripts\setup-kimi-plugin.ps1   # Windows
```

The script locates the binaries (`bin/` in a bundle, `build/` +
`mcp/target/release/` in a source tree), initializes a per-host state
directory (`~/.local/share/irez/kimi`, or `%LOCALAPPDATA%\irez\kimi` on
Windows; override with `--state-dir`), generates a Kimi personal plugin
(manifest + `.mcp.json` + the bundled `irez-investigation` skill, default
output `dist/kimi-plugin/irez/`, gitignored), merges the stdio server into
Kimi's runtime `mcp.json`, registers the plugin into the personal plugin
market when `kimi-daimon` is available (`--no-register` to skip), and verifies
with `irez-mcp doctor --stdio`. All paths embedded in the generated files are
absolute native paths of the machine running the script.

After the script succeeds: in Kimi desktop open the plugin page's **Personal**
tab and install "IREZ IR Evidence" (daemon-side install, no restart), then
start a **new** session — `mcp.json` is read at session start. Ingest with the
CLI before querying.

For reference, the runtime server list lives at
`%APPDATA%\kimi-desktop\daimon-share\daimon\runtime\kimi-code\home\mcp.json`
(POSIX: the equivalent `daimon-share/daimon/runtime/kimi-code/home/mcp.json`)
with the standard `mcpServers` shape:

```json
{
  "mcpServers": {
    "irez": {
      "command": "D:\\path\\to\\irez-mcp.exe",
      "env": {
        "IREZ_CLI": "D:\\path\\to\\irez.exe",
        "IREZ_STATE_DIR": "D:\\path\\to\\state"
      }
    }
  }
}
```

## Support scope

| Integration status | Meaning |
|---|---|
| Officially tested | Codex and OpenCode on Windows/Linux x86-64 |
| Protocol compatible | Any host supporting a local stdio MCP server (Kimi Work verified via stdio handshake) |
| Unsupported | Remote HTTP MCP, mobile hosts, and untested special sandboxes |
