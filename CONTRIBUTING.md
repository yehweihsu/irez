# Contributing to IREZ

Contributions are welcome. For a substantial feature, schema change, or new
dependency, open an issue first so the intended evidence contract and
compatibility impact can be agreed before implementation.

## Build and verify

See [docs/BUILDING.md](docs/BUILDING.md) for prerequisites and platform-specific
commands. From a configured Linux or WSL checkout, the full local gate is:

```console
make verify
make e2e
```

On Windows, build and test the native components from PowerShell:

```powershell
.\scripts\build-cpp.ps1
.\scripts\run-tests.ps1
.\scripts\build-mcp.ps1
cargo fmt --manifest-path mcp\Cargo.toml --check
cargo clippy --manifest-path mcp\Cargo.toml --locked --all-targets -- -D warnings
python scripts\generate_contract.py --check
python scripts\generate_third_party_notices.py --check
python scripts\release_notes.py --check
```

Pull requests should keep the same checks green on every affected platform.
Do not commit build directories, state databases, local toolchain paths, or
experiment output.

## Contracts and generated files

- `contract.json` is the only edit point for product and schema versions. Run
  `python scripts/generate_contract.py` after changing it. Adding a CLI command
  or MCP tool bumps the corresponding pairing contract even when its response
  envelope is otherwise additive.
- `tests/golden/v01.json` records the stable CLI contract. Re-record it only
  for an intentional behavior change, and review every changed entry:

  ```console
  python3 scripts/diff_test.py --write-golden
  python3 scripts/diff_test.py
  ```

- After changing `mcp/Cargo.lock` or a pinned native dependency, run
  `python scripts/generate_third_party_notices.py`. CI rejects stale notices.
- Keep user-visible changes in `CHANGELOG.md`. The release workflow extracts
  the matching version section and fails instead of publishing empty notes.

## Reporting bugs

Ordinary bugs may be filed as GitHub issues. Include:

- operating system, compiler, LLVM version, and whether you used a release
  bundle or a source build;
- the exact command and exit code;
- `irez status` and `irez-llvm-index version` output;
- a minimal IR reproducer when it can be shared publicly.

Remove proprietary source paths and sensitive IR before posting. Report
security issues privately as described in [SECURITY.md](SECURITY.md).
