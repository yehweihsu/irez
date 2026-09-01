# Architecture

`irez` owns parsing, SQLite state, provenance, and bounded queries. The
`irez-mcp` process owns only the stdio MCP boundary and invokes the CLI once
per tool call. Host installers are conveniences; the command, environment,
and stdio contract in `MCP_OTHER_HOSTS.md` is the stable public boundary.

Release bundles carry a machine-readable `manifest.json` with product,
platform, DB/API/analysis/adapter/LLVM, MCP, and skill versions plus SHA-256
hashes. Installed binaries live side-by-side by product version; state and
host configuration live outside those directories.
