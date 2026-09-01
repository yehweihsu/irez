# Compatibility policy

## Binary users

Official releases currently target x86-64 only:

| Bundle | Minimum runtime baseline | Build runner |
|---|---|---|
| `linux-x86_64` | glibc 2.35 (Ubuntu 22.04 class systems) | `ubuntu-22.04`, LLVM 23.1.0 |
| `windows-x86_64` | Windows 10 or newer | `windows-2022`, MSVC/VS 2022, LLVM 23.1.0 |

The Linux C++ binaries statically link the GCC support runtimes and the
Windows C++ binaries statically link the MSVC runtime. LLVM and SQLite are
linked into `irez`; users do not install an LLVM SDK, Rust, Visual Studio, or
SQLiteCpp to run a release bundle. Linux still depends on glibc and the
kernel. CI rejects a release if an ELF symbol exceeds GLIBC 2.35.

### Which IR a release binary can parse

A release binary embeds one fixed LLVM version, reported as
`llvm_build_version` by `irez status` and recorded in every artifact's
provenance. That version bounds what the binary can read:

- **IR from the same or an older LLVM major generally parses.** Bitcode has
  explicit auto-upgrade support; textual `.ll` is best-effort and has broken
  across majors before (the typed-to-opaque pointer transition being the
  well-known case).
- **IR produced by a newer LLVM major may be rejected**, with a parse error
  rather than a silent misreading. Regenerate it with a compatible Clang, or
  use a release built against that LLVM version.

Check `llvm_build_version` in `irez status` before concluding that a parse
failure is a defect in the IR. Producing IR for a project to ingest is
covered in [NEW_PROJECT.md](NEW_PROJECT.md).

`ubuntu-latest`, explicit Ubuntu 26.04, `windows-latest` (currently VS 2026),
and newer compilers remain in the test matrix as operating-system/toolchain
forward lanes.
Release binaries use LLVM 23.1.0 on the baseline runners: the LLVM reader
version and the operating-system ABI floor are independent choices.
In particular, a binary built on Ubuntu 26.04 can acquire newer GLIBC
requirements even when it uses the same LLVM 23 libraries as a build on
Ubuntu 22.04.

Windows 11, VS 2026, LLVM 23, WSL Ubuntu 26.04, and Clang/LLVM 21-23 are
supported development environments, but they are not the minimum user
requirement. LLVM 22.1.8 remains the case-reproduction toolchain for material
whose upstream defect was fixed in LLVM 23; release binaries use LLVM 23 to
read that older IR once the case is recorded.

## Source builders

| Component | Supported floor |
|---|---|
| CMake | 3.20 |
| C++ | C++20; GCC 11+, Clang 14+, MSVC from VS 2022+ |
| LLVM development SDK | 21.x through 23.x |
| Rust | 1.88+ (`mcp/Cargo.toml` enforces this) |
| Python | 3.8+ for tests and packaging only |

The production C++ code uses `std::filesystem` and otherwise conservative
C++20 facilities. `std::barrier` is confined to concurrency tests. LLVM APIs
are tested at the supported floor and release major; a new LLVM major is
added to the forward lane before being declared supported.

## Compatibility surfaces

Operating-system ABI, SQLite state schema, CLI JSON contract, MCP contract,
installer registration, adapter output, and investigation skill each have
separate versions. A change to one does not silently masquerade as another.
See [UPGRADING.md](UPGRADING.md).
