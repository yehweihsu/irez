# Progress — IREZ V00_01

Development log, newest first. Two conventions worth knowing before reading:

- **`F1`–`F6` and `A1`–`A6` are internal fix-batch numbers** from a demo
  session backlog and a pre-release checklist. Those source documents are
  local working notes and are not published; the entries below are
  self-contained, and the numbering is kept only so the batches stay
  distinguishable from each other.
- Paths outside this repository — `demo_candidates/`, `examples/`,
  `experiments/` — refer to the same unpublished local working directories.
  They are cited for provenance, not as links you can follow.

## 2026-09-01 — First hosted CI and official LLVM package setup

- The first GitHub-hosted run exposed two packaging assumptions before any
  release was created. `install-llvm-action` v2.0.9 recognizes assets only
  through LLVM 21.1.8, and its Windows `.exe` asset lacks the development
  headers, static libraries, and CMake package required by IREZ.
- Replaced that action with a repository-local setup action. Windows downloads
  the official `clang+llvm-*-x86_64-pc-windows-msvc.tar.xz` development
  archive and verifies its release SHA-256. Linux verifies the LLVM apt signing
  key, uses the official per-distribution repository, and refuses an installed
  version that differs from the exact matrix version.
- Avoided the official Linux all-project archive in IREZ CI: it nearly fills a
  hosted runner and its LTO-built static libraries cannot be linked by the GCC
  lanes as ordinary native archives. The official apt development packages are
  substantially smaller and match the successful Ubuntu/WSL build layout.
- LLVM's Ubuntu 24.04 and 26.04 repositories provide exact LLVM 23.1.0
  packages, while its Ubuntu 22.04 repository currently tops out at exact
  LLVM 22.1.8. The release matrix therefore keeps LLVM 23.1.0 for Windows and
  uses LLVM 22.1.8 for Linux rather than raising the documented glibc 2.35
  floor. Checked-in notices list both; bundle manifests and SBOMs remain
  platform-specific.
- Quoted `LLVM_DIR` so Windows paths containing spaces are passed to CMake as
  one argument, and limited push-triggered CI to `main` so Dependabot branches
  do not run duplicate push and pull-request workflows.
- The second hosted pass compiled and tested LLVM 23 successfully on both
  Ubuntu forward lanes. It also exposed two Windows export dependencies:
  LLVM's development archive references `ZLIB::ZLIB` without bundling zlib,
  and its DIA finder does not yet infer Visual Studio 2026. CMake now provides
  a SHA-256-pinned zlib 1.3.1 fallback and resolves `diaguids.lib` before
  importing LLVM targets; notices and bundle SBOM inputs include zlib.
- LLVM 21 alone retained a raw `attributes_json` field after
  `llvm::json::Object::erase`. Guard projection now removes storage-only
  columns before constructing the JSON object, preserving the same response
  contract across LLVM 21-23.

## 2026-09-01 — Ubuntu 26.04 fresh-clone verification

- Completed a fresh-clone WSL2 build and verification on Ubuntu 26.04.1 with
  GCC 15.2, CMake 4.2.3, LLVM 23.1.0, Rust 1.98, and Python 3.14. The C++ and
  Rust builds, source-quality checks, automated tests, and fixture ingest all
  completed from the committed tree.
- Replaced the non-MSVC `-idirafter` workaround with a build-local system
  include overlay that exposes only LLVM's `llvm/` and `llvm-c/` trees. This
  prevents an unversioned `/usr/include/llvm` from a different installed major
  being combined with the selected LLVM libraries, without allowing LLVM's
  adjacent libc++ `cxxabi.h` to shadow libstdc++.
- Broadened golden path normalization for diagnostics that prefix a masked
  checkout or state path. This removes the remaining Windows-versus-POSIX
  separator difference without weakening payload comparison.
- CI and release CMake configurations now set the Release build type on
  single-configuration Linux generators. CI also has an Ubuntu 26.04 / GCC /
  LLVM 23 forward lane with an end-to-end fixture ingest.
- Forced Cargo metadata JSON decoding to UTF-8. Dependency author names now
  generate identically on Windows and Linux instead of depending on the
  Windows ANSI code page.

## 2026-08-31 — LLVM 23 release toolchain, contract, and licensing

- Selected LLVM 23.1.0 for release binaries so the embedded reader covers
  the widest currently supported IR range. LLVM 22.1.8 remains the
  case-reproduction toolchain for material whose upstream defect is fixed in
  LLVM 23.
- Verified a native Windows build with LLVM 23.1.0, Visual Studio 2026 / MSVC
  19.51, and Ninja. LLVM 23's Windows archive requires
  `zstd::libzstd_static` but does not ship zstd, so CMake now builds a pinned,
  SHA-256-verified zstd 1.5.7 fallback on Windows.
- Fixed Windows environment import when a parent process contains both
  `PATH` and `Path`; the stale entry previously hid `cl.exe` and silently
  selected a MinGW compiler. Also fixed the `build-cpp.ps1 -NoTests` boolean
  conversion before it reached CMake.
- Resolved the additive-command policy: a new CLI command or MCP tool bumps
  its pairing contract. `cli_contract_version` and `mcp_contract_version`
  are now 2 for `trace-stores` / `irez_trace_stores`; the resolved item was
  removed from `OPEN_QUESTIONS.md`.
- Python is now a required configure-time dependency when tests are enabled;
  `diff-golden` can no longer disappear from an otherwise green CTest run.
- Removed the explicit `windows-2025-vs2026` matrix entry: GitHub lists the
  label as GA but omits it from the private-repository standard-runner table,
  while `windows-latest` already supplies VS 2026. This keeps the private
  release rehearsal schedulable without losing VS 2026 coverage.
- LLVM-version-dependent `body_fingerprint` values are normalized by the
  differential harness until the fingerprint is made structurally stable.
  Full LLVM 23 CTest is green: 57 GoogleTests and all 47 golden commands.
- Confirmed Apache-2.0 for IREZ with `Copyright 2026 Yewei Xu`; the official
  license text remains unmodified. Added `NOTICE` and a generated
  `THIRD_PARTY_NOTICES.md` containing the locked Rust and pinned native
  dependency notices. CI and release jobs reject a stale generated file.
- Release manifests now carry a platform-specific third-party component
  inventory. SPDX output uses the actual declared and selected license
  expressions, includes zstd and SQLite's public-domain `LicenseRef`, and no
  longer emits `NOASSERTION` for dependency licenses. A real Windows bundle
  and its 76-package SPDX document passed the release verifier.
- Added a user-facing `CHANGELOG.md` and concise `CONTRIBUTING.md`; BUILDING,
  Makefile, CI, installation, and binary packaging now share the same release
  documentation and generated-file checks. GitHub releases take notes from
  the changelog instead of relying on an empty single-commit comparison.
- Release-candidate tags such as `v0.1.0-rc.1` are explicitly prereleases.
  Their bundle `version` retains the suffix while compiled `irez_version` and
  `skill_version` remain `0.1.0`. Both stable and RC Windows archives passed
  the 36-file manifest verifier; the RC SBOM contains 76 packages and no
  dependency license `NOASSERTION` values.
- Completed the pre-Git public-boundary scan. The local-only build helper and
  private planning documents are held outside the public source tree; no
  additional private document, secret, CJK text, unfinished marker, or
  unexplained machine path remains in the repository. `AGENTS.md` is
  intentionally absent because no current process depends on its obsolete
  first-design content.

## 2026-08-16 — Kimi Work (Kimi desktop) as a third host

- New `scripts/setup-kimi-plugin.sh` / `scripts/setup-kimi-plugin.ps1`:
  one-shot Kimi configuration from a source tree or an extracted release
  bundle. They locate the binaries (`bin/` or `build/` + `mcp/target/release/`),
  init a per-host state dir (`~/.local/share/irez/kimi` or
  `%LOCALAPPDATA%\irez\kimi`), generate a Kimi personal plugin
  (`dist/kimi-plugin/irez/`: manifest + `.mcp.json` + bundled
  `irez-investigation` skill) with machine-local absolute native paths
  (Git Bash paths are converted with `cygpath -w`; PowerShell writes JSON
  without a BOM), merge the stdio entry into Kimi's runtime `mcp.json`,
  register into the personal plugin market when `kimi-daimon` is present,
  and finish with `irez-mcp doctor --stdio`.
- Verified on Windows: both scripts generate a plugin that passes the Kimi
  plugin validator (0 errors/warnings) and a doctor run that is green
  (13 tools, sample `irez_status` call) against a fresh state dir ingested
  with all of `fixtures/`.
- `docs/MCP_OTHER_HOSTS.md` Kimi section rewritten around the scripts;
  `docs/MCP_SETUP.md` supported-hosts line updated.

## 2026-08-16 (later) — new-project onboarding doc

Docs assumed a ready `.ll` file; nothing covered producing IR from C/C++
sources or moving a host between projects. New `docs/NEW_PROJECT.md`:
clang `-S -emit-llvm -g` per translation unit (debug info is what makes
`source_mapping` evidence available; optimization level is the user's
choice), one state dir per project, catalog-vs-full ingest, host pointing
(Kimi: `setup-kimi-plugin` re-run or `mcp.json`; Codex/OpenCode:
`install --state-dir --force`), and the two switching workflows
(flip-the-pointer vs combined state). Linked from README and QUICKSTART.

## 2026-08-16 (evening) — demo-session fix backlog F1-F6

Source: `../demo_candidates/IREZ_FIX_BACKLOG.md` — six issues observed in three
live demo sessions (choose fixture, candidate-issue hunting, jax#38602 ingest).
All six fixed in one round; verification at the bottom.

- **F2 (P0) `trace-stores`**: kernel-shaped functions (XLA/Numba/Triton) end in
  `ret void` / `ret ptr null` and deliver results through `store`, which
  `trace-return` could not reach. `trace_return` was refactored into thin
  sink-collecting wrappers over a shared `run_trace` core (same per-site
  payload, budgets, truncation contract), and a new `trace-stores` command /
  `irez_trace_stores` MCP tool traces every store's stored value backward,
  reporting `store`, `value` (+`value_shape`), and `pointer` per site. As a
  companion, `trace_return` flags the kernel shape in `unknowns`
  (`kind: result_channel`, hint to `trace-stores`) when no return carries a
  real value and the function stores. MCP tool count 13 → 14
  (`EXPECTED_TOOL_COUNT`, smoke test list, docs). New fixture
  `fixtures/store_kernel.ll` (`fmul→fsub→fdiv→store`, `ret ptr null`).
  Diagnostics keys generalized while touching them: `budget_nodes_per_return`
  → `budget_nodes_per_sink`, truncation scope `per_return` → `per_sink`, mixed
  reason label `per_return_budget` → `per_sink_budget`.
- **F5 (P1) include semantics**: `include=[...]` now EXTENDS the `detail`
  default section set (the documented "add flags on top" behavior) instead of
  replacing it — `detail=graph + include=["flags"]` previously dropped
  nodes/relations silently. Regression tests `TraceReturnIncludeExtendsDetail
  Default` / `TraceReturnIncludeFlagsKeepsGraphProjection`; the old
  override-semantics test was rewritten. Skill `tools.md`, `docs/CLI.md`, and
  the MCP tool description updated to match.
- **F1 (P0) state_dir split-brain**: three layers. (1) `ingest`/`reindex`
  responses now echo the resolved absolute `state_dir` (lowest-cost stopgap
  from the backlog); `init` also reports the canonical path now. (2) `status`
  scans the other well-known state locations (`./.irez`,
  `%LOCALAPPDATA%\irez\*` / `~/.local/share/irez/*`) and emits a
  `state_dir_conflict` warning diagnostic when one holds a database — on this
  machine it correctly fires against the installed Kimi host state.
  (3) `irez-mcp` logs a startup warning when `IREZ_STATE_DIR` is unset (its
  `./.irez` fallback otherwise differs from the installed-host default).
  The diff-golden normalizer excludes `state_dir_conflict` entries — they are
  environment facts, not response contract.
- **F3 (P1) argument-order errors**: exit-2 error JSON now carries a `usage`
  field with the correct syntax for the command being parsed
  (`IrezError::usage`, attached by the CLI dispatcher). `ingest llvm --index
  full PATH` now answers with `usage: irez ingest llvm PATH [--index
  catalog|full] [--refresh]` instead of only "expected --index, got full".
- **F4 (P1) implicit target**: an empty `functions` result produced with the
  implicitly defaulted (most recent) artifact now carries an
  `implicit_target` diagnostic naming the applied scope and the `--artifact`
  hint; explicit scopes and non-empty results stay quiet.
- **F6 (P2) count semantics**: `status` always carries a
  `materialization_counts` diagnostic stating that `materialized_functions`
  counts storage-level `structure_ready` rows (query-triggered on-demand
  materialization included once the query completes) and pointing at
  `functions` for per-function state.

### Verification

- C++: 56/56 unit tests (7 new: trace-stores chain/empty, kernel-shape hint,
  include×2, implicit-target, count-semantics; 1 updated for the per_sink
  rename, 1 rewritten for additive include).
- diff-golden regenerated with `--write-golden` and reviewed: 3 new commands
  (store-kernel ingest + both traces) and 11 changed labels, all matching the
  intended contract changes above; `all 42 commands match` afterwards.
- Rust: `cargo fmt --check`, `clippy -D warnings`, `cargo test --locked`
  (smoke suite drives all 14 tools) green.
- End-to-end on a scratch state dir: F1 state_dir echo + conflict warning,
  F3 usage field, F4 diagnostic, F5 graph-preserved flags, F2 store chain
  (`store → fdiv → fsub → fmul → load`) and the `result_channel` hint all
  confirmed against the CLI binary.

### Notes

- Contract versions in `contract.json` intentionally unchanged: every change
  is additive at the field level (new command/tool, new envelope fields,
  widened include semantics). Whether an additive command warrants a
  cli/mcp contract bump is recorded as an open question.
- This run initially attributed local VS 18 discovery trouble to a non-default
  install location and used an unpublished machine-local helper. The
  2026-08-31 pass identified the actual cause as duplicate `PATH` / `Path`
  environment entries, fixed the generic discovery script, and removed the
  helper from the public tree.
- The fixes reach installed hosts only after a release rebuild and
  `install --force` / `setup-kimi-plugin` re-run.

### Deployment (later the same day)

The F1-F6 fixes are deployed to the three local hosts:

- Release bundle: `dist/irez-0.1.0-windows-x86_64.zip` (C++ core +
  `irez-mcp`; the `irez-mcp` release build lives in `mcp/target-pkg/`
  because the old `mcp/target/release/irez-mcp.exe` was locked by 5
  running host processes).
- **Kimi**: after stopping the old processes, replaced
  `mcp/target/release/irez-mcp.exe` and re-ran
  `scripts/setup-kimi-plugin.ps1` — mcp.json updated, doctor green
  (14 tools); the plugin was re-registered into the personal market as
  **0.1.1** (the market cannot pick up an update without a version bump;
  the generated `kimi.plugin.json` was bumped manually, and the setup
  script's "13 tools" description was corrected to 14). Confirm the
  update on the plugin page's "Personal" tab and open a new session for
  it to take effect.
- **Codex**: `irez-mcp install codex --force` (the codex CLI lives at
  `%LOCALAPPDATA%\OpenAI\Codex\bin\<hash>\codex.exe`, not on PATH);
  doctor all green (binary staged to `%LOCALAPPDATA%\irez\versions\0.1.0\`).
- **OpenCode**: `irez-mcp install opencode --force`; doctor all green.

## 2026-08-16 — post-deploy live verification + English sweep

- Live-verified the deployed 0.1.1 build against the real XLA kernel from the
  F2 backlog scenario (`artifact:5b9cf76276827808`,
  `module_0007.jit_f.divide_bitcast_fusion_kernel_module.ir-with-opt.ll`,
  function `divide_bitcast_fusion`): `trace_return` reports the
  `result_channel` unknown (ret-void kernel shape) with a hint to use
  `trace-stores`; `trace_stores` returns the full
  `store ← fdiv ← {fsub, fmul} ← loads/GEPs` chain (16 nodes, 19 relations,
  no truncation).
- English-only sweep for open-source-facing files: translated the remaining
  Chinese segments in `docs/PROGRESS.md`,
  `experiments/fld1-rerun/REPORT.md`, `experiments/fld1-rerun/REPORT.stdio-run.md`,
  and `.gitignore` comments. A full-tree CJK scan (excluding build/target/dist)
  now reports clean. Source comments, `skills/`, and other docs were already
  English. No code or golden output changed, so no rebuild/re-record was
  needed; `skills/` was untouched, so no plugin re-publish was needed.
  Development notes outside the repo (e.g. `demo_candidates/`) intentionally
  remain in Chinese.

## 2026-08-16 (later) — A1: OpenCode Windows default path fix

Release-checklist item A1: the installer defaulted to
`%APPDATA%\opencode\opencode.jsonc` on Windows, but OpenCode reads
`%USERPROFILE%\.config\opencode\opencode.jsonc` — install + doctor went green
while the host never saw the tools (exactly the silent-failure class this
project exists to reject). The 2026-08-16 deployment had indeed landed in the
wrong location (confirmed on this machine).

- `default_opencode_config` now resolves `<home>/.config/opencode/
  opencode.jsonc` on Windows (HOME then USERPROFILE, mirroring `user_home`;
  %APPDATA% no longer consulted) and returns `Result` instead of silently
  falling back to `.`. Resolution logic factored into the pure, env-injectable
  `default_opencode_config_from`; the skill destination follows the config
  parent, so it lands in `~/.config/opencode/skills/irez-investigation`
  without a separate change. `--opencode-config` override unchanged.
- New unit tests (5): Windows USERPROFILE placement, HOME-over-USERPROFILE
  precedence, missing-home error; Unix XDG_CONFIG_HOME precedence and HOME
  fallback. cargo fmt/clippy/test (8 unit tests) and the real-stdio smoke are
  green.
- CI Windows lane gained a default-path installer smoke (no
  `--opencode-config`): asserts the config lands under the redirected
  USERPROFILE, never under APPDATA, and that the skill is installed.
- Machine acceptance: install --force wrote the config to the correct path
  (timestamped backup), owned skill with marker installed, doctor all green
  (14 tools); uninstall removed only installer-owned content (opencode.json,
  other skills, and backups untouched), then reinstalled. Stale wrong-path
  artifacts from the earlier deployment moved to
  `%LOCALAPPDATA%\irez\a1-cleanup-backup-20260816\`; the pre-existing unowned
  skill copy was moved aside as `irez-investigation.bak-20260816`.
  Remaining: host-visibility after an actual OpenCode restart (user action).
- The release build went to `mcp/target-a1/` because running host processes
  still lock `mcp/target/release/irez-mcp.exe`; the staged binaries under
  `%LOCALAPPDATA%\irez\versions\0.1.0\bin\` (shared by Codex and OpenCode)
  now carry this fix.

## 2026-08-16 (evening) — A2: exit-code contract table + golden probes

Release-checklist item A2: `docs/CLI.md` documented exit codes 0/2/3/1/7,
but the implementation (`src/error.h`) never returns 1 and uses 4/5/6.

- CLI.md table now lists 0/2/3/4/5/6/7 with the same meanings as
  `error.h`, states that exit code 1 is never used, and notes that the
  stderr JSON always carries `error`/`error_kind`/`exit_code`. No
  "exit 1"/"other failure" residue remains in docs/ or README.
- Exit-code contract coverage completed: new golden probes
  `ingest-malformed-ir` (exit 4, `fixtures/malformed.ll`) and
  `ingest-unsupported-txt` (exit 6, `fixtures/not_ir.txt`), appended at the
  end of the golden sequence so the intentionally-failed ingests cannot
  perturb earlier entries. Golden now covers 0/2/3/4/5/6; exit 7 stays
  covered by the `LiveMaterializationClaimReturnsStructuredBusy` unit test.
  Golden suite 42 → 44 commands, `diff_test.py` 44/44 green.

## 2026-08-16 (night) — A6: outward-facing text finalized

Release-checklist item A6 (README is the storefront):

- Positioning sentence updated to the calibrated line: "IREZ is a bounded,
  provenance-preserving LLVM IR evidence layer for white-box audits by
  engineers and AI agents", explicitly naming ML/inference-infrastructure,
  performance, numerics, framework, and systems engineers (not
  compiler-engineers-only). "LLVM 21's C++ API" generalized to "LLVM's C++
  API", consistent with the "21+ (tested 21.x and 22.x)" requirement note.
- "An owned skill is atomically updated" replaced with an accurate
  description of the implementation: fully staged into a sibling temp
  directory, then swapped in via rename after the old copy is removed, with
  the crash window and reinstall recovery stated plainly.
- `mcp/tests/smoke.rs` comment "all 12 irez tools" corrected to 14 (the
  assertion list already had 14; only the comment was stale).
- Repo-wide sweep for overclaims (production-grade / proves-absence /
  all-queries-exact / atomic) is clean; the surviving "atomic publication"
  in the build section describes the artifact store's flush+rename and is
  accurate.

## 2026-08-16 (night, cont.) — A3: explicit scope boundary for global slice

Release-checklist item A3: slicing a module-scoped entity (e.g. `@g` in
`fixtures/globals.ll`) returned `nodes:1 / relations:0` with no explanation —
with the default relation set the CD-capability unknown happened to be
present, but a selection without `control` produced literally empty unknowns,
and nothing said *why* the slice was empty (the same silence class as
V00_00 B2).

- `Service::slice_on` now appends a `kind=scope` unknown whenever the target
  has no function_id: slice traversal is function-scoped, only the target
  node is returned, and `uses`/`show` are pointed out for direct neighbors
  of module-scoped entities. `truncated=false` is kept; artifact-wide global
  traversal remains a separate design (budget + function boundaries), not a
  filter removal.
- Regression at three layers: new unit test `GlobalSliceReportsScopeBoundary`
  (both directions, relation selection without control-dependence so the CD
  unknown cannot mask a missing scope report); golden probes
  `ingest-globals` / `slice-global-forward` (default relations → scope + CD
  unknowns) / `slice-global-backward` (`operand,llvm.references-global` →
  scope only); CLI.md's slice row now states the function-scoped semantics.
  Unit tests 56 → 57, golden 44 → 47, all green.
- Note: `references-global` has no short alias; the CLI selection needs the
  canonical `llvm.references-global` (aliases cover only
  operand/control/call/cfg).
## 2026-08-04 — self-describing state, boundary honesty, harness cleanup

- `status` now reports `state_dir` (resolved absolute path, forward slashes on
  every OS). A wrong `--state-dir`/`IREZ_STATE_DIR` previously failed
  silently — artifact IDs are content hashes, so two state directories look
  alike until counts seem off. This was the fld1 rerun's misdiagnosis root
  cause. Golden normalization masks it as `<STATE>`.
- Call-boundary honesty fix in `trace_return`: a `llvm.calls` edge whose
  target has no entity row was labeled `internal` + `expandable=true`,
  sending callers to a doomed expand. It now reports `status="unknown"`,
  `expandable=false`, `reason="target_not_indexed"`. Every boundary also
  carries `reason` (the target's materialization state), distinguishing
  "declaration only" (genuinely external) from "definition indexed but not
  yet materialized" — both have module-local function ids, so `status` alone
  was ambiguous. The boundary query no longer selects the unused raw
  `attributes_json` column.
- Harness cleanup: `scripts/diff_test.py` no longer compares against the
  historical IREZ_V00_00 Python prototype (`--compare-live` removed together
  with `tests/golden/python.json`); V00_01 C++/Rust is the only
  implementation under test. `docs/BUILDING.md` and `tests/CMakeLists.txt`
  updated to match.
- Non-bug design items from the rerun review (artifact GC, truncation resume
  cursor, chain topology, explicit `encoding_lossy=false`) are recorded in
  `docs/OPEN_QUESTIONS.md` instead of being implemented speculatively.
- Contract surfaces aligned: MCP `irez_status`/`irez_trace_return`
  descriptions, Skill (`tools.md`, `workflows.md`, `experiments.md`),
  `docs/CLI.md`; Kimi Work skill copy resynced, release `irez-mcp` rebuilt.

Verification: 50/50 unit tests green (new: `StatusReportsResolvedStateDir`,
`TraceReturnCallBoundaryReasonAndUnknownTarget`); diff-golden "all 39
commands match"; MCP `cargo test` / `clippy -D warnings` / `fmt --check`
clean; CLI smoke confirms `state_dir` shape.

## 2026-08-04 — flags section and summary-by-default (fld1 rerun review)

Driven by the fld1 rerun's round-trip analysis: 4 of the follow-up calls were
per-instruction `show` queries whose only purpose was reading fast-math flags,
and the `detail` default contradicted the recommended workflow.

- New opt-in `flags` trace-return section: sparsely lists graph nodes carrying
  non-default boolean instruction attributes (nsw, exact, fast-math family)
  as `{handle, opcode, flags}` rows. Only instruction nodes are considered —
  function/argument nodes' own boolean attributes (e.g. `declaration`) are
  not instruction flags. A requested section with no flagged nodes returns an
  empty array, so "checked, none set" is distinguishable from "not requested".
  Default payloads are unchanged; unknown sections remain exit-2 usage errors.
- `detail=summary` is now the default everywhere (service, CLI, MCP when the
  argument is omitted), matching the documented summary-first workflow;
  `detail=graph` is the explicit opt-in for nodes/relations.
- Contract surfaces aligned: MCP arg docs and tool description, Skill
  (`tools.md`, `workflows.md`), `docs/CLI.md`; the Kimi Work skill copy and
  the release `irez-mcp` binary were rebuilt/resynced.
- Golden coverage: new `trace-return-graph` (explicit opt-in) and
  `trace-return-flags` commands; the plain `trace-return` golden now records
  the summary default. 39 commands total.

Verification: 48/48 unit tests green (3 new: sparse flags rows, empty-array
contract, summary-default/graph-opt-in); diff-golden "all 39 commands match";
MCP `cargo test` / `clippy -D warnings` / `fmt --check` clean; CLI smoke
confirms the summary default and the two-row nsw flags answer on
`nonfloating.ll`.

## 2026-08-03 — trace-return projection and harness tool-call counting (round 3)

- `trace-return` gains a response projection: `--detail graph|summary` plus an
  explicit `--include chain,calls,source,nodes,relations,boundaries` override.
  `summary` (recommended first call for agents) replaces the graph payload
  with a compact `chain` spine plus per-site `node_count`, `relation_count`,
  and `value_shape` (`scalar`/`vector`/`void`). Unknown detail/include values
  are usage errors (exit 2), never a silent fallback.
- `call_boundaries` entries now carry `target_name` and `target_llvm_type`,
  so a summary call identifies the callee without a follow-up `show`.
- Per-site `truncation` is emitted in every projection — it is a correctness
  contract, not payload. Diagnostics echo the applied `detail`/`sections`.
- MCP `irez_trace_return` exposes `detail`/`include`; the tool description,
  server instructions, Skill (`tools.md`, `workflows.md`, `experiments.md`,
  `SKILL.md`), and `docs/CLI.md` all steer agents to `detail="summary"` first
  and forbid repeating an identical call to re-extract fields.
- New `scripts/count_tool_calls.py` counts MCP `tools/call` events from
  captured JSON/JSONL host logs (total, per-tool, exact-duplicate groups with
  canonical argument keys); `docs/EXPERIMENT.md` now measures tool-call counts
  from logs instead of agent self-report.
- Golden coverage: three new diff-golden commands (`--detail summary`,
  `--include chain,nodes`, unknown-section usage error). The legacy
  `trace-return` golden gained only the new contract fields
  (`detail`/`sections` diagnostics, `value_shape`, counts, named boundaries).

Verification: 46/46 unit tests green (4 new projection tests); diff-golden
"all 37 commands match"; MCP `cargo test` / `clippy -D warnings` /
`fmt --check` clean; CLI smoke confirms summary/include/error shapes.

## 2026-08-03 — bounded investigation and protocol-hardening sprint

- The CLI JSON boundary sanitizes malformed UTF-8, marks replacement-bearing
  responses `encoding_lossy`, and the MCP smoke suite drives all 13 tools,
  including an LLVM identifier containing invalid UTF-8 bytes.
- Function `show` now defaults to a bounded summary. Complete IR requires
  `--view exact`; bounded block/return/call discovery uses `--view children`.
- Entity results expose canonical `handle` while retaining `id`; MCP inputs
  accept `handle`, `id`, or `entity_id` and reject conflicting aliases.
- Runtime status separates IREZ, DB, API, analysis, adapter, and LLVM build
  versions. The prototype rejects unsupported old or future DB schemas without
  modifying them; stale analysis semantics trigger reanalysis within DB v2.
- Added the bounded function-local `trace-return` composed evidence query and
  rewrote the investigation Skill/tool descriptions around user questions
  instead of full-function containment browsing.

## 2026-08-03 — review-driven hardening: honesty bugs, contract single-sourcing, build provenance

Driven by the fld1 rerun review (agent round-trip analysis): the interaction
design held up; these are the productization fixes.

### Correctness fixes

- **guards silent truncation (honesty invariant).** Both guards queries used
  `LIMIT budget` and always reported `truncated=false`, silently dropping
  guards beyond the budget; `budget=0` returned an empty "exact" result with
  no signal at all. Queries now fetch `budget + 1` (same probe pattern as
  `uses`), overflow rows become `boundaries` (terminator handles), and the
  envelope reports `truncated=true, reason=node_budget`. Regression tests:
  `GuardsBudgetZeroReportsTruncationHonestly`,
  `GuardsBeyondBudgetAreReportedAsBoundaries`.
- **trace_return per-site truncation.** The composed query merged every
  return site's boundaries and kept only a total `visited_nodes` plus a
  blanket `per_return_budget` reason, forcing callers to *infer* which site
  hit which budget. Each site now carries its own `truncation` record
  (`truncated/reason/visited_nodes/budget_nodes/budget_depth`) and its own
  `boundaries`; a single reason across sites is promoted verbatim to the
  top-level envelope (mixed reasons still collapse to `per_return_budget`),
  and `diagnostics` gains a `{kind: truncation, scope: per_return,
  truncated_sites, total_visited_nodes}` summary. Regression test:
  `TraceReturnPreservesPerSiteTruncation`.
- **Windows artifact ingest was unconditionally broken.** `durable_sync`
  opened the artifact temp file `_O_RDONLY` and then called `_commit`;
  FlushFileBuffers requires write access, so every ingest on Windows failed
  with "cannot flush artifact temporary file". Now opened `_O_RDWR`.
- **Cross-platform response contract.** Source file paths are canonicalized
  to forward slashes in the adapter (Windows emitted mixed separators), and
  `functions --match` no longer embeds standard-library-specific
  `std::regex_error::what()` text in its error (stable ECMAScript message).

### Version/capability contract single-sourcing

- New `contract.json` is the single source of truth for `irez_version`,
  schema versions, adapter id/version, CLI/MCP contract versions, and the
  six-entry materialization capability set. `scripts/generate_contract.py`
  derives `src/contract.h` and `mcp/src/contract.rs` and rewrites the
  `mcp/Cargo.toml` version line; `CMakeLists.txt` reads `irez_version` from
  `contract.json` directly; `package_release.py` reads schema versions from
  it and refuses a mismatch. A version bump is now: edit `contract.json`,
  run the generator, rebuild. CI runs `--check` so stale generated files
  fail fast.
- The materialization cache-validity check no longer compares a bare
  capability count (`>= 6`): it requires the completed capability set to be
  exactly the contract set (names and precisions), so a foreign or partial
  six-row manifest is treated as stale and rematerialized. Regression tests:
  `ForeignCapabilityManifestTriggersRefresh`,
  `MaterializeWritesExactContractCapabilitySet`.
- `guards` responses never leak the raw `attributes_json` storage column
  (regression test `GuardsNeverLeakRawAttributesJson`).

### Build provenance and doctor

- CMake bakes `IREZ_BUILD_REVISION` (git short hash, `unknown` without VCS
  info) into the binary; `irez status` reports it as `build_revision`.
- The installer ownership manifest now records contract versions from the
  generated constants plus FNV-1a fingerprints of the CLI binary, the MCP
  binary, and the embedded skill payload (`fingerprint_algorithm: fnv1a64`,
  a change detector, not a security boundary; release bundles keep SHA-256).
- `doctor` (`irez-mcp ... check`) now parses the `irez status` JSON and
  cross-checks every recorded contract version against the running CLI,
  verifies CLI/MCP binary fingerprints against the install record, verifies
  the embedded skill fingerprint, and cross-checks a sibling
  `irez-llvm-index`'s `adapter_version` against the CLI's. Mixed builds that
  merely share a "0.1.0" string now fail loudly.
- `package_release.py` manifests include `build_revision` (env
  `IREZ_BUILD_REVISION` > git > `unknown`).

### Verification

- Windows/MSVC: `ctest` 2/2 (42 unit tests incl. 6 new regressions;
  diff-golden regenerated with exactly the 6 intentional diffs).
- Rust: `cargo test --locked` green, `clippy -D warnings` clean,
  `rustfmt --check` clean.
- `generate_contract.py` bump/revert round-trip verified (`0.1.0` → `0.1.1`
  → back, `--check` green throughout).

### Known remaining items from the review (not in this round)

- `trace_return` projection (`detail=summary|graph`, `include=[...]`) to
  eliminate repeated full-trace calls.
- Backend-bounded slice traversal (indexed adjacency queries per frontier
  instead of loading all artifact entities/relations).
- Benchmark harness should count tool calls from its event log, not from
  the evaluated agent's self-report.

## 2026-08-03 (later) — backend-bounded slice traversal

Review item 4 ("bounded output" is not "bounded backend work"): `slice`
loaded **every relation of the function** and **every entity of the
artifact** into memory before running its 50-node/depth-8 BFS, so response
size was bounded but database reads and memory were not — and `trace_return`
repeated that load once per return site, opening a fresh connection per
slice and source call.

- The BFS now reads only the current frontier node's adjacency through the
  `relations_src_kind` / `relations_dst_kind` indexes (`WHERE function_id=?
  AND <frontier>=? AND kind IN (...) ORDER BY kind,ordinal,src_id,dst_id`;
  ordering matches the old in-memory comparator, NULL ordinals first).
- Entities are fetched afterwards, only for the visited ids, in bounded
  500-id `IN (...)` batches.
- `slice_on` / `source_on` private overloads take a caller-provided
  connection; `trace_return` runs all per-site slice/source/call queries on
  its single connection. Public `slice()`/`source()` are thin wrappers.

Benchmark (same state, 1500-function module, 124,501 entities / 187,499
relations, tracing 16 nodes; pre-change binary rebuilt from the untouched
source copy, new binary from this tree):

| command | before | after |
|---|---|---|
| `trace-return --budget-nodes 50 --budget-depth 8` | 1.914 s | 0.056 s |
| `slice --budget-nodes 50 --budget-depth 8` | 1.740 s | 0.048 s |

~35x faster on this module; the old cost grows with artifact size, the new
cost is proportional to the visited subgraph. As a side-by-side contract
check, the old binary reported `reason: per_return_budget` for this trace
while the new one reports `depth_budget` plus the per-site truncation record
from the previous round.

Verification: 42/42 unit tests green; diff-golden "all 34 commands match" —
responses are byte-identical to the pre-change implementation on every
fixture command (traversal order preserved).

## 2026-08-02 — distribution and GitHub CI/CD pass

- Apache-2.0 licensing, versioned Windows/Linux x86-64 bundles, embedded hash
  manifests, deterministic archive metadata, archive verification, SPDX 2.3
  SBOMs, SHA-256 release checksums, and optional GitHub provenance.
- CI separates Ubuntu 22.04/Windows Server 2022 runtime baselines from latest
  and VS 2026 forward lanes; LLVM 21/22, GCC, Clang, MSVC, Rust MSRV, MCP
  handshake, and OpenCode installer lifecycle are exercised.
- Linux releases enforce a GLIBC 2.35 ceiling and static GCC support runtimes;
  Windows releases use the static MSVC runtime. Newer local systems are test
  environments, not accidental runtime requirements.
- Windows orchestration is PowerShell-first and imports the VS developer
  environment through `vswhere`/`VsDevCmd`; batch files are compatibility-only.
- FetchContent inputs are immutable/hash-verified, dependency install leakage
  is suppressed, Actions are SHA-pinned, and Dependabot maintains Cargo and
  workflow dependencies.

## 2026-08-02 — release-readiness state/provenance pass

- Schema v2 adds catalog state/claims, requested and completed index levels,
  analyzer identity, and explicit per-function capability completion. The
  original v1 migration path is superseded by the prototype policy above:
  unsupported old/future schemas are identified and rejected without mutation.
- Content storage now writes the bytes used for hashing to a durable temporary
  file and atomically publishes them. Dedup verifies the backing file.
- Dedup no longer suppresses recovery, adapter upgrades, catalog-to-full
  upgrades, `--refresh`, or `reindex`. Adapter failures persist failed runs and
  recoverable state instead of poisoning a content hash.
- Materialization uses a cross-process claim with busy timeout, WAL, stale-claim
  recovery, atomic snapshot replacement, fail-closed record validation, and
  explicit exact-empty completion evidence.
- Control dependence now walks the post-dominator tree to the branch IPDom.
  Tests cover exact-empty, multi-block arms, loops, multi-exit,
  infinite/unreachable regions, and switch case evidence.
- Slice preserves relation attributes; fake call depth was removed; CLI enums
  and budgets are validated; read commands never create a missing database.
- The installer embeds the skill, uses ownership manifests and platform home
  roots, and performs a real initialize/tools-list/status-call MCP self-check.
  Setup scripts no longer swallow install failures.
- Differential testing now uses an independent exact V00_01 contract golden;
  optional Python differences are allowed only by JSON field path.
- CMake dependency versions and Rust MSRV are pinned, with Linux/Windows LLVM
  21/22 CI coverage.

## 2026-08-02

- Audited IREZ_V00_00 (Python + C++ adapter): confirmed the layering and
  envelope contract are sound; found 12 bugs (B1-B12, listed at the end of this file),
  empirically reproducing B1 (internal call expand misreported as external),
  B2 (silent empty slice on catalog-only functions), and B3 (unconditional br
  reported with a bogus predicate).
- Created IREZ_V00_01: C++20 core (store/service/envelope/adapter in-process),
  `irez` CLI, standalone `irez-llvm-index` JSONL binary. SQLite via vendored
  SQLiteCpp (bundled amalgamation); tests via vendored GoogleTest; both
  referenced through `IREZ_DEPS_DIR` (default `../Software_Repos`).
- Ported all 13 CLI commands with identical envelope schema, exit codes, and
  handle format; bug fixes B1-B10 and B12 carried in.
- `scripts/diff_test.py`: runs a 31-command sequence through both the Python
  and C++ implementations against fresh state directories, normalizes
  volatile values (uuid/timestamps/paths), and compares. Result: 23 commands
  byte-identical after normalization, 8 intentional diffs (all registered
  with bug ids). Wired into ctest as `diff-golden`.
- GoogleTest suite (17 tests): ports of the V00_00 store/envelope/integration
  tests plus regression tests for B1-B9 and B12. All green.
- New fixtures: `internal_call.ll` (B1), `diamond.ll` (B4), `globals.ll` (B6).
- Fixed porting-time bugs the Python original never had: non-owning
  llvm::json StringRef leaves corrupting source records, use-after-move in
  `source()`, uppercase content hashes.
- CMake workaround: Debian/Ubuntu LLVM 21 exports reference
  `zstd::libzstd_shared` without defining it when libzstd-dev is missing;
  the target is now synthesized from `find_library` before `find_package(LLVM)`.
- Rust MCP server (`mcp/`): `irez-mcp`, rmcp 3.1.0 over stdio, 11 tools as
  thin spawn wrappers around the CLI, structured-content responses with the
  envelope verbatim, CLI error envelopes surfaced as `isError` tool errors.
  End-to-end smoke test (`mcp/tests/smoke.rs`) drives a real 2026-07-28
  handshake: initialize, tools/list (exactly the 11 tools), irez_status, and
  the CLI error path. `cargo clippy` clean.
- Installer (`irez-mcp install|check|uninstall codex|opencode`) replaces
  agent_setup.py: per-host state init, `codex mcp add` registration, OpenCode
  JSONC config editing with timestamped backups and a real comment/trailing
  -comma scanner (B11 — strings containing `//` or `/*` are preserved),
  skill linking, non-destructive uninstall. Verified against a synthetic
  HOME for both install and uninstall.
- Noted for operators: rmcp's `ProtocolVersion::LATEST` is 2025-11-25 while
  2026-07-28 exists in KNOWN_VERSIONS; sessions established via the classic
  initialize handshake work unchanged, and the SDK handles the 2026-07-28
  wire-shape differences (resultType discriminator, error-code upgrades)
  automatically.
- Field test (OpenCode 1.18.5): a full investigation flow worked, and it
  surfaced a real gap — MCP exposed no artifact listing, so agents could not
  discover artifact handles when more than one artifact was ingested. Added
  `irez_artifacts` (12th tool) and documented the multi-artifact workflow in
  the skill references.

## 2026-08-02 (evening) — native Windows (MSVC) build + open-source prep

- First native Windows build: MSVC (VS 2022+ C++ workload, via vswhere) +
  CMake/Ninja + the official `clang+llvm-*-x86_64-pc-windows-msvc.tar.xz`
  development archive (LLVM 22.1.8; the `LLVM-*-win64.exe` installer ships no
  C++ headers/libs and cannot be used). Rust MCP server built with the
  stable `x86_64-pc-windows-msvc` toolchain. GoogleTest suite and e2e smoke
  (init/ingest/functions) all pass on Windows.
- New `scripts/win_env.bat` (shared VS/CMake/Ninja discovery; no machine
  paths) plus rewritten `build_cpp.bat`, `build_mcp.bat`, `run_tests.bat`;
  optional env overrides: `IREZ_LLVM_DIR`, `IREZ_DEPS_DIR`, `BUILD_DIR`,
  `CMAKE_BUILD_TYPE`.
- CMake: prebuilt LLVM archives hard-code the build machine's DIA SDK path
  (`diaguids.lib` in `LLVMDebugInfoPDB`'s interface); `CMakeLists.txt` now
  detects the dangling path and retargets it to the local DIA SDK
  (`IREZ_DIAGUIDS_LIBRARY` override available).
- diff-golden was red on Windows for one environment artifact: the checked-in
  golden had been recorded under WSL, so `original_path` was a `/mnt/...`
  path. `scripts/diff_test.py` now masks the checkout root as `<ROOT>` and
  canonicalizes path separators on both sides; the golden file itself is
  machine-neutral. `diff-golden` is green on Windows; WSL/Linux behavior
  unchanged. `python_runner` now inherits the environment and defaults the
  adapter to this tree's `build/irez-llvm-index` (overridable via
  `IREZ_LLVM_ADAPTER`), so goldens can be recorded on Windows too.
- Open-source prep: purged machine-specific paths from Makefile (`TMPDIR`
  variable), `examples/fld1/README.md`, golden, and all scripts; added
  `docs/BUILDING.md` (Linux/WSL/Windows prerequisites, LLVM package notes,
  troubleshooting) and linked it from the README build section. `.gitignore`
  now covers logs and `__pycache__`.
- Toolchain-version policy recorded: require LLVM 21+ (tested 21.x/22.x),
  any C++20 compiler; nothing in the tree is sensitive to LLVM/MSVC/Clang
  minor versions.
- GCC 15 fix: LLVM 21's Ubuntu package bundles a libc++-oriented
  `<cxxabi.h>` that conflicts with libstdc++ when it shadows the toolchain
  headers (breaks `test_core.cpp` via gtest). LLVM include dirs are now
  passed with `-idirafter` on GCC/Clang (`SYSTEM` on MSVC), so the
  toolchain's own C++ headers always win. Verified: clean WSL build
  (GCC 15.2, LLVM 21.1.8) + full ctest green; Windows rebuild unaffected.
- Registered `irez-mcp` with Windows OpenCode (`install opencode --cli
  ...\build\irez.exe`): server entry written to
  `~/.config/opencode/opencode.jsonc` (timestamped backup), per-host state
  dir initialized at `~/.local/share/irez/opencode`, fixtures ingested via
  the CLI. Note: the installer refuses to replace an existing skill link;
  the already-linked `irez-investigation` skill is current.
- OpenCode field test (Windows): the full 12-tool loop verified from the
  host — status/artifacts/functions/materialize/context/slice over
  `nonfloating.ll`, correctly reporting the diamond CFG of `choose`, the
  `llvm.control-dependence` unsupported unknown, and the conservative
  guards capability.
- One-shot host setup scripts: `scripts/setup-mcp-host.sh` (Linux/WSL) and
  `scripts/setup-mcp-host.ps1` (Windows). Both build whatever is missing,
  register with opencode (default) or codex, tolerate the
  already-linked-skill no-op, and finish with `check`. `IREZ_BUILD_DIR`
  (sh) / `BUILD_DIR` (ps1) override the CLI build location — needed when a
  checkout is shared between Windows and WSL (one CMake cache per
  platform). New `docs/MCP_SETUP.md` documents the installer semantics,
  per-platform state/config paths, state preparation, verification prompt,
  and troubleshooting; README MCP section now points to it. Verified
  end-to-end: ps1 on Windows (re-registration, tolerant path) and sh in
  WSL (durable CLI at `~/irez-v01-build`, `--force` replacing a stale
  registration, `check` green).


## 2026-08-02 (night) — llvm.control-dependence

- Adapter now computes exact control dependence at materialize time
  (Ferrante–Ottenstein–Warren over `llvm::PostDominatorTree`): block Y is
  control-dependent on a terminator via successor edge ordinal S iff that
  successor post-dominates Y but not the branch block. Emitted as
  `llvm.control-dependence` relations (block-anchored src, terminator dst,
  successor block in attributes) with precision `exact`; the adapter
  capabilities record advertises `control_dependence` supported/exact.
- `guards` upgraded: artifacts with CD evidence get exact answers — the
  branches a block is control-dependent on, required successor edge, and the
  reconstructed branch predicate (conditional `br` only; unconditional `br`
  keeps predicate null, B3). A block that post-dominates its region
  correctly reports no guards. Legacy state directories (no CD evidence)
  fall back to conservative CFG-predecessor evidence, still flagged
  `partial/conservative` with an explicit unknown.
- `slice` now traverses real CD edges (CLI default relations are
  `operand,control`); the unsupported unknown is only reported when the
  artifact carries no CD evidence. Note the edges are block-anchored:
  instruction-level "what controls me" is `guards`' job.
- Tests: `ControlDependenceDiamondEdges`, `GuardsExactViaControlDependence`,
  `SliceTraversesControlDependence`, and the B3 regression moved to the
  legacy fallback (CD rows deleted to simulate a pre-CD state).
  `diff_test.py` registers 4 new intentional diffs (capabilities,
  materialize-choose, slice-default, slice-declaration) and updates the
  guards-phi reason. Windows (MSVC) and WSL (GCC 15) both 2/2 green.

## Appendix — bugs fixed during the V00_00 to V00_01 port

The two sections below are topical rather than dated: they collect the
defects carried in from the V00_00 audit and the ones found during the
port itself.

## Bug fixes carried in from the V00_00 audit

Each fix has a regression test in `tests/test_core.cpp`. B1-B4, B6-B8 change
observable behavior; `scripts/diff_test.py` records the intentional diffs
against the Python golden in its `INTENTIONAL_DIFFS` table.

- B1 `expand`: V00_00 selected `r.*, e.kind, e.attributes_json`; the duplicate
  column names made `call["kind"]` read the relation kind (`llvm.calls`), so
  every internal call was misreported as `external`. The C++ port uses
  explicit column aliases; internal calls now materialize the callee.
- B2 lazy materialization: `slice`/`uses`/`guards`/`source` silently returned
  empty results for catalog-only functions; only `show` materialized on
  demand. All of them now call `ensure_materialized` first.
- B3 `guards`: an unconditional `br` had its destination block reported as
  `predicate`. A predicate is now only reported for a conditional `br`
  (exactly two CFG successors).
- B4 `slice`: a node reached at maximum depth was listed as a truncation
  boundary even when already visited (e.g. the shared operand of a diamond).
  Only unexplored nodes become boundaries; edges between known nodes are
  kept. Verified with `fixtures/diamond.ll`.
- B5 provenance of failures: V00_00 created the analysis-run row inside the
  load transaction, so a failure rolled the run back too. The run row is now
  committed first and marked `failed` with `error_json` in a separate
  transaction. (Adapter-level parse failures still happen before run
  creation and are reported as exit code 4 without a run row.)
- B6 entity ownership: materialization assigned `function_id` to `global`
  entities, letting the last materialized function "own" a module global.
  Globals keep `function_id NULL` (`fixtures/globals.ll`).
- B7 declarations: V00_00 marked declarations `structure_ready` at ingest.
  They are now `declaration_only`; `materialize` on a declaration reports
  that status instead of faking readiness, and `status` no longer counts
  them as materialized functions.
- B8 deduplicated ingest: V00_00 re-ran the adapter catalog and re-upserted
  all entities for already-known content. A deduplicated ingest now only
  reads the existing rows (no new analysis run).
- B9 envelope consistency: `ingest` returned `capabilities_used` as a
  one-element list containing the raw capabilities dict; it is now the same
  `[{name,status,precision}]` list shape every other command uses. `expand`
  now reports `command: "expand"` (it previously leaked `"materialize"`).
- B10 adapter robustness: `emitSource` dereferenced `DILocation::getScope()`
  without a null check (malformed debug info could crash); dead ordinal maps
  were removed; the standalone binary keeps the `version` subcommand.
- B11 agent_setup.py stripped JSONC with regexes, eating `//` inside string
  values; the replacement installer (`irez-mcp install`) uses a real
  string/comment state-machine scanner with unit tests.
- B12 media type: `.BC`/`.LL` now classify by the lowercased suffix in both
  the accept check and the stored media type.

Known intentional difference that is not a bug fix: regex error message
wording differs (`std::regex` vs Python `re`); the exit code (5) matches.

## New bugs found and fixed during the port itself

- llvm::json::Value constructed from StringRef/const char* is non-owning
  (T_StringRef). Adapter records referencing module strings dangled after
  the module was destroyed, which corrupted `source` records. All adapter
  emissions now go through `own_json`, as do envelope leaves copied from
  parsed JSON. Watch for this whenever a `json::Value` crosses a scope
  boundary.
- `source()` evaluated `rows.empty()` after moving `rows` into the envelope,
  so it always emitted a spurious `source_mapping unavailable` unknown.
- Content-hash hex: `llvm::toHex` is uppercase, Python `hexdigest` lowercase.
  Hashes are lowercased to keep handles identical across implementations.
