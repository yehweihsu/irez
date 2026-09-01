# Onboarding a new project

How to go from "I have a C/C++ codebase" to "the Agent can investigate it
through IREZ". Three steps: produce LLVM IR, prepare a state directory, point
your host at it. Also covers switching between projects.

IREZ ingests **LLVM IR files**, not C/C++ sources: each `.ll` (textual IR) or
`.bc` (bitcode) file becomes one immutable artifact. It parses them with the
LLVM libraries it was built against, so IR produced by a much newer Clang may
be rejected; regenerate it with a compatible Clang in that case. See
[COMPATIBILITY.md](COMPATIBILITY.md#which-ir-a-release-binary-can-parse) for
the exact rule and how to check your binary's LLVM version.

## 1. Produce LLVM IR with Clang

One IR file per translation unit, next to the build or in a scratch dir:

```bash
# textual IR, with debug info (recommended)
clang -S -emit-llvm -g -O0 foo.c -o foo.ll
clang++ -S -emit-llvm -g -O0 bar.cpp -o bar.ll

# bitcode works too
clang -c -emit-llvm -g foo.c -o foo.bc
```

Practical notes:

- **`-g` matters**: without debug info, `source_mapping` evidence (which IR
  instruction came from which source line) is unavailable. Everything else
  (CFG, operand graph, control dependence) works without it.
- **Optimization level is your choice**: `-O0` shows the code as written;
  `-O1`/`-O2` show what the optimizer produced. IREZ reads the IR statically
  and never runs the optimizer itself. Ingest both if you want to compare.
- **Many files**: a project yields one `.ll` per translation unit. Produce
  them all, e.g. `for f in src/*.c; do clang -S -emit-llvm -g "$f" -o "ir/$(basename "$f" .c).ll"; done`.
  For build-system-driven projects, `gllvm`/`wllvm` (Linux) or intercepting
  the compile commands also work; anything that ends with valid LLVM IR files
  is fine.
- Declarations without bodies (headers, externals) are cataloged as
  `declaration_only`; only defined functions can be materialized.

## 2. Prepare a state directory

Use **one state directory per project** — it is the unit you will point hosts
at, back up, and delete:

```bash
IREZ=/path/to/irez            # bundle: ./bin/irez ; source tree: build/irez
STATE=~/.local/share/irez/myproject        # Windows: %LOCALAPPDATA%\irez\myproject

$IREZ --state-dir "$STATE" init --name myproject
for f in ir/*.ll; do
  $IREZ --state-dir "$STATE" ingest llvm "$f" --index full
done
```

- `--index catalog` is cheaper (names/signatures only); `--index full` adds
  per-function analysis up front. Functions can always be materialized on
  demand later, so `catalog` is fine for a first pass over a large project.
- Re-running `ingest` on an unchanged file is a no-op (content-addressed);
  a changed file gets a new artifact id. Use `reindex` after an IREZ upgrade
  to recompute analysis from the stored immutable artifact.

Verify before involving the Agent:

```bash
$IREZ --state-dir "$STATE" status
$IREZ --state-dir "$STATE" artifacts
$IREZ --state-dir "$STATE" functions --match main
```

`status` should report your artifact/function counts with
`failed_catalogs: 0`.

## 3. Point the host at the state

The MCP server reads exactly one state directory, via `IREZ_STATE_DIR`.

- **Kimi Work**: re-run `scripts/setup-kimi-plugin.sh --state-dir "$STATE"`
  (or edit `IREZ_STATE_DIR` in the runtime `mcp.json`), then start a new
  session.
- **Codex / OpenCode**: `irez-mcp install <host> --state-dir "$STATE" --force`,
  then restart the host.

## Switching between projects

There is no in-session switch; the state directory is fixed per host
registration. Two workflows:

1. **Flip the pointer** (one active project at a time): redo step 3 with the
   other project's state dir, new host session. Both states stay on disk;
   nothing is re-ingested.
2. **Combine into one state** (compare across projects): ingest both
   projects' IR into the same state dir. Artifacts are content-addressed and
   never collide; use `irez_artifacts` / `functions --artifact` to scope
   queries. Best for small related modules (e.g. before/after pairs, as in
   [EXPERIMENT.md](EXPERIMENT.md)).

A stale or schema-incompatible state is never modified by the server: old
states from an older IREZ are refused without changes; create a fresh state
and re-ingest the original IR files. See [UPGRADING.md](UPGRADING.md).
