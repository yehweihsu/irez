# Changelog

This file records user-visible changes to IREZ. The detailed engineering log
is kept in [docs/PROGRESS.md](docs/PROGRESS.md).

## [Unreleased]

## [0.1.0] - 2026-08-31

### Added

- A C++20 LLVM IR evidence service with persistent SQLite state, deterministic
  entity handles, selective function materialization, and machine-readable
  JSON envelopes.
- Catalog, exact entity inspection, uses, source mapping, bounded graph and
  slice traversal, guard evidence, investigation context, return tracing, and
  store-sink tracing.
- Explicit capability, precision, boundary, unknown, truncation, provenance,
  and version evidence on every response.
- A thin Rust MCP server exposing 14 query tools through stdio, plus installers
  and diagnostics for Codex, OpenCode, and Kimi.
- Linux and Windows x86-64 release bundles, reproducible archive metadata,
  SHA-256 checksums, SPDX 2.3 SBOMs, and public-build provenance attestations.
- An investigation skill and documentation for agent-guided, bounded LLVM IR
  analysis.

### Compatibility

- Source builds support LLVM 21 through 23; official 0.1.0 binaries use LLVM
  23.1.0 so the embedded reader covers the widest tested IR range.
- State, API, analysis, adapter, CLI, MCP, registration, and skill compatibility
  are versioned independently. Unsupported state schemas are refused without
  modification.
- Rust 1.88 is the minimum supported Rust toolchain for `irez-mcp`.

### Known limitations

- Indirect calls remain unknown/conservative, and memory dependencies are not
  modeled.
- Runtime observations cannot be inferred from SSA and require a future
  external evidence importer.
- IREZ analyzes supplied IR; it does not diff optimization pipelines or
  attribute a semantic change to a particular compiler pass.
- Official binary bundles are currently limited to Linux and Windows x86-64.

[Unreleased]: https://github.com/yehweihsu/irez/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yehweihsu/irez/releases/tag/v0.1.0
