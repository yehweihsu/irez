# IREZ V00_01

[![ci](https://github.com/yehweihsu/irez/actions/workflows/ci.yml/badge.svg)](https://github.com/yehweihsu/irez/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

IREZ is a bounded, provenance-preserving LLVM IR evidence layer for white-box
audits by engineers and AI agents. It parses textual LLVM IR and bitcode
through LLVM's C++ API, persists a selective structural graph in SQLite, and
exposes the same response envelope through a JSON CLI and a thin MCP server.

Canonical repository: <https://github.com/yehweihsu/irez>.

## The kind of question this answers

[llvm/llvm-project#186922](https://github.com/llvm/llvm-project/issues/186922):
a loop copying `i32` values is incorrectly vectorized. The driver prints `100`
at `-O0` and `107` at `-O2`.

```bash
opt reduced.ll -S -o reduced_optimized.ll \
  -passes='inline,loop-rotate,sroa,instcombine,loop-vectorize'
clang -O0 main.c reduced_optimized.ll && ./a.out   # expect 100, got 107
```

The `main.c` driver is twenty lines and obviously correct. A source-level
indexer has full visibility into it and still has nothing to say, because the
defect is introduced by a pass pipeline that runs after the source is gone.
The fix, when it landed, was in Loop Access Analysis — an IR-level analysis
reaching an IR-level conclusion.

Questions in that region — which pass changed the meaning of this loop, which
operands actually feed this value in *this* build, is this call boundary
opaque or did the analysis give up — need the IR itself as evidence.
[docs/WHY_IR.md](docs/WHY_IR.md) develops this case and five more real ones,
including the limits of the argument.

IREZ does not replace source-level tooling. For "where is this function
called", use clangd.

## What a response looks like

Every response carries its own limits. This is `trace-return` on the bundled
`fixtures/nonfloating.ll`, abridged:

```json
{
  "command": "trace-return",
  "capabilities_used": [
    { "name": "direct_calls",   "precision": "exact",   "status": "supported" },
    { "name": "operand_graph",  "precision": "exact",   "status": "supported" },
    { "name": "source_mapping", "precision": "partial", "status": "supported" }
  ],
  "result": {
    "function": "irez:5b72ac864e894652:llvm:function:f1",
    "return_count": 1,
    "sites": [
      {
        "call_boundaries": [
          {
            "target_name": "external",
            "status": "external",
            "reason": "declaration_only",
            "precision": "exact",
            "modality": "must",
            "expandable": false
          }
        ]
      }
    ]
  },
  "diagnostics": [
    { "kind": "truncation", "scope": "per_sink",
      "total_visited_nodes": 8, "truncated_sites": 0 }
  ],
  "evidence_refs": ["artifact:5b72ac864e894652", "run:..."],
  "unknowns": []
}
```

`precision` separates exact structural facts from partial ones. A call that
leaves the indexed set is reported as a boundary with the reason it is one,
not silently dropped. Truncation is stated rather than implied by a short
answer. Nothing here claims a solved path condition.

## Install a binary release

Download and extract the Windows or Linux x86-64 bundle from
[the latest release](https://github.com/yehweihsu/irez/releases/latest).

On Linux/WSL, extract the `.tar.gz` from inside the Linux environment so its
POSIX executable bits are preserved. A Windows archive tool writing directly
into the WSL filesystem may create `bin/*` as non-executable; see
[Quick start](docs/QUICKSTART.md) for the exact command and recovery step.

The supported golden path is:

```bash
./bin/irez-mcp install codex       # or: opencode
./bin/irez-mcp doctor codex
```

The installer uses the sibling `irez` binary, copies both executables to a
versioned per-user directory, initializes state, registers absolute paths, and
installs the embedded investigation skill.

See [Quick start](docs/QUICKSTART.md), [supported hosts](docs/MCP_SETUP.md),
[onboarding a new project](docs/NEW_PROJECT.md), or the
[host-agnostic stdio contract](docs/MCP_OTHER_HOSTS.md). Updates and
schema/skill compatibility are in [UPGRADING.md](docs/UPGRADING.md); when
something goes wrong, start at
[TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## CLI quickstart

For an extracted binary release (`.exe` is implied on Windows):

```bash
./bin/irez --state-dir /tmp/demo init --name demo
./bin/irez --state-dir /tmp/demo ingest llvm fixtures/nonfloating.ll --index catalog
./bin/irez --state-dir /tmp/demo functions --match choose
./bin/irez --state-dir /tmp/demo materialize function '<function-handle>'
./bin/irez --state-dir /tmp/demo show '<function-handle>'
./bin/irez --state-dir /tmp/demo trace-return '<function-handle>' \
  --budget-nodes 50 --budget-depth 8
```

From a source checkout, replace `./bin/irez` with `build/irez`.

See [docs/CLI.md](docs/CLI.md) for the full command reference, envelope
contract, handle format, and exit codes. V00_01 adds bounded function views,
`trace-return`, refresh/reindex behavior, and explicit capability/version
evidence; `--adapter` is gone because the parser is in-process.
`irez-llvm-index` (catalog/function/version subcommands, JSONL on stdout)
remains available for debugging.

User-visible release history is recorded in [CHANGELOG.md](CHANGELOG.md).

## Design decisions

- DB layout, API envelope, analysis semantics, adapter, and LLVM build are
  versioned independently. Unsupported old or future DB schemas are refused
  without modification in the prototype release.
- SQLite access goes through a vendored SQLiteCpp build using its bundled
  sqlite3 amalgamation; tests use vendored GoogleTest. Both are referenced
  through the `IREZ_DEPS_DIR` CMake variable instead of being copied, so
  Linux/Windows and x64/ARM64 builds only need a C++20 compiler, CMake,
  LLVM 21+, and Cargo.
- `irez-llvm-index` remains as a standalone JSONL binary with the V00_00
  command-line contract, for debugging and adapter cross-checks. The CLI
  itself drops `--adapter`: there is no external parser process anymore.
- `irez-mcp` spawns the `irez` CLI per tool call and passes the JSON envelope
  through. No FFI, no duplicated logic; the CLI stdout contract is the IPC.
- UUIDs are generated with `std::random_device` (RFC 4122 v4); no dependency.

V00_01 is a full rewrite of the V00_00 service layer: **C++20 owns all core
logic** (store, queries, envelope, and the LLVM adapter, now linked
in-process), and **Rust owns only the MCP interface**. There is no Python left
in the product path. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the
process boundaries and [docs/PROGRESS.md](docs/PROGRESS.md) for the work log
and the V00_00 bugs fixed during the port.

## Layout

```text
src/            C++ core + CLI + standalone adapter (all of the logic)
mcp/            irez-mcp: Rust MCP server (rmcp, stdio), spawns the CLI
tests/          GoogleTest suite + Python golden for the differential test
scripts/        diff_test.py (C++ vs Python differential harness)
fixtures/       LLVM IR fixtures (including regression fixtures)
skills/         agent investigation skill (language-agnostic)
```

## MCP for developers

```bash
make mcp          # or .\scripts\build-mcp.ps1 on Windows
IREZ_STATE_DIR=/absolute/path/to/state IREZ_CLI=/absolute/path/to/build/irez \
  mcp/target/release/irez-mcp
```

The server speaks MCP (stdio transport, protocol versions up to 2026-07-28)
and exposes 14 query tools (`irez_status`, `irez_artifacts`, `irez_functions`,
`irez_show`, `irez_graph`, `irez_slice`, `irez_uses`, `irez_guards`,
`irez_context`, `irez_source`, `irez_expand`, `irez_capabilities`,
`irez_trace_return`, `irez_trace_stores`). It holds no state and contains no
query logic: each tool call spawns the CLI and returns its envelope. As in
V00_00, MCP intentionally offers no `init` or `ingest` tool; prepare the state
directory with the CLI first.

Host registration (OpenCode, Codex) is a one-shot step per platform:

```bash
scripts/setup-mcp-host.sh                          # Linux/WSL
powershell -File scripts\setup-mcp-host.ps1        # Windows
```

which wraps the lower-level installer:

```bash
mcp/target/release/irez-mcp install codex --cli "$PWD/build/irez"     # or opencode
mcp/target/release/irez-mcp doctor codex --cli "$PWD/build/irez"
mcp/target/release/irez-mcp uninstall codex
```

The installer initializes a per-host state directory
(`~/.local/share/irez/<platform>` by default), registers the server with the
host (Codex via `codex mcp add`, OpenCode via `opencode.jsonc` with a
timestamped backup and a real JSONC scanner — not regexes), and installs the
`irez-investigation` skill into the host's personal skill directory. An owned
skill is fully staged into a sibling temporary directory and then swapped in
with a rename after the old copy is removed — a crash in that window can leave
the skill absent (reinstall to recover); unowned content is never replaced.
See [docs/MCP_SETUP.md](docs/MCP_SETUP.md) for per-platform details, state
preparation, verification, and troubleshooting. Uninstall removes only what
the installer created; state and artifacts are kept.

## Build from source

Building from source needs an LLVM development SDK (21.x through 23.x) and a Rust
toolchain. On Windows it additionally needs Visual Studio with the C++
workload and the LLVM archive that ships the development files — not the
ordinary installer. **If you just want to try IREZ, use the binary release
above**; that is the supported path, and the source build exists for
contributors.

Linux / WSL:

```bash
make build     # C++ core, CLI, standalone adapter, tests
make test      # ctest: GoogleTest suite + Python golden differential test
make e2e       # init + ingest --index full against fixtures/
make mcp       # cargo build --release of mcp/ (irez-mcp)
```

Windows (PowerShell + MSVC; Visual Studio is discovered via `vswhere`):

```powershell
.\scripts\build-cpp.ps1
.\scripts\run-tests.ps1
.\scripts\build-mcp.ps1
```

Dependencies:

- C++20 compiler (GCC, Clang, or MSVC), CMake >= 3.20; GNU Make on Linux/WSL
- LLVM 21+ development files (tested with 21.x through 23.x; not sensitive to the
  exact minor version)
- Rust toolchain (>= 1.88) for the MCP server
- GoogleTest 1.17.0, SQLiteCpp 3.3.3, and the Windows LLVM 23 zlib/zstd
  fallbacks are found or fetched automatically; `IREZ_DEPS_DIR` supports
  pinned offline checkouts
- Python 3 is required when tests are enabled; it is never used at runtime

The same sources build on Linux, WSL, and Windows; official binary releases
currently target Windows/Linux x86-64 only. SQLite comes from SQLiteCpp's
bundled amalgamation; the artifact store uses the native flush primitive on
each platform before atomic publication.

See [docs/BUILDING.md](docs/BUILDING.md) for prerequisites per platform, LLVM
package notes (including which Windows archive ships the development files),
configuration variables, and troubleshooting. Runtime and toolchain floors are
in [COMPATIBILITY.md](docs/COMPATIBILITY.md); maintainers should also read the
[release procedure](docs/RELEASING.md). For a bounded, copy-paste release
experiment in a fresh Agent window, see [EXPERIMENT.md](docs/EXPERIMENT.md).

## Known limitations

- `guards` reports exact control dependence (post-dominator analysis) for
  artifacts ingested with the current adapter; older state directories
  without control-dependence evidence fall back to immediate CFG predecessor
  evidence flagged `partial/conservative`. Neither mode claims solved path
  conditions.
- Indirect calls are unknown/conservative and memory dependencies are
  unsupported.
- Runtime observations are not collected from SSA. Context packets explicitly
  report them as unavailable unless a future external evidence importer
  supplies them.
- IREZ indexes the artifacts you give it. It does not diff pass pipelines or
  attribute a change to a pass; producing the before/after IR pair is your
  job.

Copyright 2026 Yewei Xu. Licensed under the [Apache License 2.0](LICENSE).
Third-party licenses and attributions are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
