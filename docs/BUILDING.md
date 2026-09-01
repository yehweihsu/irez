# Building IREZ

IREZ builds from the same sources on Linux, WSL, and Windows without edits.
Official bundles currently cover Linux and Windows x86-64; other targets are
source-only. There are two independently buildable pieces:

- the C++ core, `irez` CLI, `irez-llvm-index` adapter, and GoogleTest suite
  (CMake);
- the `irez-mcp` MCP server (Cargo), which spawns the CLI and needs no C++
  toolchain of its own beyond a linker.

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| C++ compiler | C++20 | GCC 11+, Clang 14+, or MSVC (VS 2022 17.x+) |
| CMake | >= 3.20 | dependencies below are pinned |
| LLVM development files | 21+ | tested with 21.x through 23.x; the build is not sensitive to the exact minor version |
| Rust | >= 1.88 | only for `mcp/`; enforced by Cargo metadata |
| Python 3 | >= 3.8 | required when tests are enabled and for release tooling; never at runtime |
| GoogleTest + SQLiteCpp | 1.17.0 / 3.3.3 | installed, fetched, or supplied offline |

The build uses a deliberately small LLVM API surface (`Core`, `IRReader`,
`BitReader`, `Analysis`, `Support`), but compatibility is verified rather
than assumed. See [COMPATIBILITY.md](COMPATIBILITY.md) for the baseline/latest
matrix and binary ABI floor.

The Windows release configuration was last verified locally on 2026-08-31
with Visual Studio 2026 / MSVC 19.51, Ninja, LLVM 23.1.0, Rust 1.88, and
Python 3.12. The release workflow continues to test LLVM 21 as the source
compatibility baseline. Local WSL results are recorded here only after a
fresh-clone verification, never inferred from the native Windows build.

The Ubuntu configuration was verified on 2026-09-01 with Ubuntu 26.04.1 LTS
x86-64 under WSL2, GCC 15.2, CMake 4.2.3, LLVM 23.1.0, Rust 1.98, and Python
3.14. Native Ubuntu uses the same commands. CI has an explicit Ubuntu 26.04 /
GCC / LLVM 23 lane in addition to the Ubuntu 22.04 compatibility baseline.

### C++ dependencies

The default build finds matching installed packages and otherwise fetches
GoogleTest 1.17.0 by immutable commit and SQLiteCpp 3.3.3 from a SHA-256
verified source archive. Offline builds can point
`IREZ_DEPS_DIR` at a directory containing `GoogleTest/` and `SQLiteCpp/`
checkouts of those versions. On Windows, an optional `zstd/` checkout avoids
fetching the pinned zstd 1.5.7 fallback required by LLVM 23. SQLiteCpp bundles
sqlite3.

```bash
# One-time setup, from the repository root (any directory works — just pass
# -DIREZ_DEPS_DIR=... or set the environment variable accordingly):
mkdir -p ../Software_Repos
git clone --branch v1.17.0 --depth 1 https://github.com/google/googletest.git ../Software_Repos/GoogleTest
git clone --branch 3.3.3 --depth 1 https://github.com/SRombauts/SQLiteCpp.git ../Software_Repos/SQLiteCpp
# Optional Windows/LLVM 23 offline dependency:
git clone --branch v1.5.7 --depth 1 https://github.com/facebook/zstd.git ../Software_Repos/zstd
```

The Rust MCP server depends only on crates.io crates (`rmcp`, `tokio`, ...);
no vendored Rust dependencies are required.

## Linux / WSL

### Ubuntu 26.04 with LLVM 23

Install the native build prerequisites. Rustup is recommended because IREZ
requires Rust 1.88 or newer and the distribution's `cargo` package is not the
version contract used by this repository.

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build llvm-23-dev libclang-23-dev \
  libzstd-dev python3 curl

# Skip this when `rustc --version` is already 1.88 or newer.
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"

llvm-config-23 --version       # expected: 23.x
rustc --version                # expected: 1.88 or newer
python3 --version              # expected: 3.8 or newer
```

From the repository root, select LLVM 23 explicitly and build all components:

```bash
export LLVM_DIR="$(llvm-config-23 --cmakedir)"

make build                    # C++ core, CLI, adapter, and tests
make mcp                      # release-mode irez-mcp
make verify                   # complete quality and automated-test gate
make e2e                      # fresh state plus full fixture ingest
```

`make build` produces:

```text
build/irez
build/irez-llvm-index
build/tests/irez-tests
mcp/target/release/irez-mcp   # after make mcp
```

Confirm that the adapter was compiled against the intended LLVM installation:

```bash
build/irez-llvm-index version
# dialect_version, llvm_build_version, and llvm_ir_reader_version should be 23.x
```

Initialize a state directory, ingest IR, and run a query:

```bash
build/irez --state-dir /tmp/irez-demo init --name demo
build/irez --state-dir /tmp/irez-demo ingest llvm fixtures/nonfloating.ll --index full
build/irez --state-dir /tmp/irez-demo functions --match choose
```

GoogleTest and SQLiteCpp are fetched at their pinned versions when they are
not installed or present under `IREZ_DEPS_DIR`. For a repeatable offline
build, populate `../Software_Repos` as described under C++ dependencies before
running CMake.

### Other Debian/Ubuntu versions

Use the same sequence with a supported LLVM major and its matching package
suffix, for example `llvm-21-dev`, `libclang-21-dev`, and `llvm-config-21`.

```bash
export LLVM_DIR="$(llvm-config-21 --cmakedir)"
make verify
make e2e
```

Notes:

- Set `LLVM_DIR` explicitly when more than one LLVM major is installed. The
  configure step isolates the selected LLVM headers so an unversioned
  `/usr/include/llvm` from another major cannot be mixed with its libraries.
- On Debian/Ubuntu, LLVM's CMake exports may reference
  `zstd::libzstd_shared` without defining it when `libzstd-dev` is absent.
  `CMakeLists.txt` synthesizes the imported target in that case; installing
  `libzstd-dev` also works.
- WSL behaves identically to native Linux. Windows paths are reachable under
  `/mnt/<drive>/`, but keeping the checkout on the Linux filesystem is faster.

## Windows (MSVC)

Requirements:

- Visual Studio 2022 or newer with the *Desktop development with C++*
  workload (includes MSVC, CMake, Ninja, and the DIA SDK);
- an LLVM 21+ development package — use the official
  `clang+llvm-<version>-x86_64-pc-windows-msvc.tar.xz` archive from the LLVM
  GitHub releases page (the `LLVM-<version>-win64.exe` installer is a
  toolchain only and does **not** ship the C++ headers, static libraries, or
  CMake config files). Unpack it anywhere, e.g. under `IREZ_DEPS_DIR` as
  `LLVM-<version>/`;
- Rust via [rustup](https://rustup.rs) (`x86_64-pc-windows-msvc` toolchain);
- Python 3 for the differential test.

Build from PowerShell (no developer prompt needed). The scripts locate Visual
Studio through `vswhere` and import the x64 `VsDevCmd.bat` environment:

```powershell
.\scripts\build-cpp.ps1
.\scripts\run-tests.ps1
.\scripts\build-mcp.ps1
.\scripts\package-release.ps1 -Version 0.1.0
```

The underscore-named `.bat` files remain thin compatibility wrappers and
contain no toolchain or configuration logic.

Configuration via environment variables (all optional):

| Variable | Default | Purpose |
|---|---|---|
| `IREZ_LLVM_DIR` | auto-detected | LLVM prefix containing `lib\cmake\llvm`; when unset, `build-cpp.ps1` scans `IREZ_DEPS_DIR\LLVM-*` |
| `IREZ_DEPS_DIR` | `..\Software_Repos` | directory with `GoogleTest\`, `SQLiteCpp\`, and optional `zstd\` checkouts |
| `BUILD_DIR` | `build\` | build output directory |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | CMake build type |

The resulting binaries are `build\irez.exe`, `build\irez-llvm-index.exe`,
`build\tests\irez-tests.exe`, and `mcp\target\release\irez-mcp.exe`.

Notes:

- The official prebuilt LLVM archives hard-code the DIA SDK path of the
  machine that produced them (`diaguids.lib`, referenced by
  `LLVMDebugInfoPDB`). `CMakeLists.txt` detects a dangling path and retargets
  it to the local DIA SDK automatically; if detection fails, set
  `IREZ_DIAGUIDS_LIBRARY` to the full path of `diaguids.lib`.
- Prefer `cmake --build` with the Ninja generator (the scripts default to it
  when available) for the fastest builds; the Visual Studio generator also
  works.

## Verifying the build

On Linux or WSL, `make verify` is the complete source-quality and test gate.
It checks Rust formatting and Clippy, generated contract constants,
third-party notices, release-note extraction, Python syntax, the C++ tests and
golden replay, and Rust/MCP tests against the built CLI. `make e2e` adds a
fresh state initialization and full fixture ingest.

```bash
make verify
make e2e
```

The corresponding Windows commands are listed in
[CONTRIBUTING.md](../CONTRIBUTING.md#build-and-verify); the native build and
test scripts remain the authoritative orchestration on that platform.

```bash
# Linux/WSL                              :: Windows (cmd)
build/irez --state-dir /tmp/demo init --name demo
build/irez --state-dir /tmp/demo ingest llvm fixtures/nonfloating.ll --index catalog
build/irez --state-dir /tmp/demo functions --match choose
```

On Windows use `build\irez.exe` and a state directory such as
`build\demo-state`.

`ctest` runs two tests: the GoogleTest suite (`irez-tests`) and
`diff-golden`, which replays a fixed command sequence through the C++ CLI and
compares against the checked-in golden after normalizing volatile values
(UUIDs, timestamps, checkout paths, build revisions). The golden is
machine-neutral; path spellings from Linux, WSL (`/mnt/...`), and Windows
(`D:\...`) all normalize to the same placeholders.

To re-record or extend the golden after an intentional contract change:

```bash
python3 scripts/diff_test.py --write-golden   # record from the C++ implementation
python3 scripts/diff_test.py                  # compare C++ vs golden
```

Every diff is a failure: review the golden diff in git whenever you re-record.
The legacy `--compare-live` mode against the IREZ_V00_00 Python prototype was
removed; V00_01 (C++/Rust) is now the only implementation under test.
