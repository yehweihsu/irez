# Why IR-level evidence

Some correctness questions cannot be asked about source code, because the
thing that is wrong is not in the source code. This document collects real,
publicly documented examples of that class, and states plainly what IREZ does
and does not contribute to them.

Issue states below were checked on **2026-08-30**. They change; the argument
does not depend on any individual issue still being open.

## The distinction

Source-level indexers — clangd, ctags, an LSP server, a code-search tool —
answer questions about declarations, references, types, and call hierarchies
derived from the AST. Within that scope they are precise and fast, and IREZ
does not replace them.

They cannot answer a question about a *compiled artifact*, because the
artifact is not derivable from the source alone. The same translation unit
produces different IR, and different behavior, depending on optimization
level, target CPU, and compiler version. When the defect is introduced
between the AST and the machine code, "read the source harder" has no
remaining move: the source is already correct, and every reviewer who looks
at it agrees.

The questions in that region look like:

- Which pass changed the meaning of this loop, and what did the IR look like
  on either side of it?
- This value is wrong at run time. Which operands actually feed it in *this*
  build, as opposed to the ones the source implies?
- Is this call boundary opaque to the optimizer, or did the analysis simply
  give up?

Answering them requires the IR itself as evidence.

## Case 1 — C source is correct, clangd is present, and it still cannot help

**[llvm/llvm-project#186922](https://github.com/llvm/llvm-project/issues/186922)**
— *miscompilation: incorrect vectorization of loop copying i32 values*.
Reported 2026-03-16 by the Zig project. **Closed**: fixed by
[#187023](https://github.com/llvm/llvm-project/pull/187023), *[LAA] Catch
load/store to invariant address in dependency checker*, merged to `main` on
2026-04-01. It reproduced on LLVM 21 and 22.

A loop copying `i32` values is incorrectly vectorized. The driver prints
`100` at `-O0` and `-O1`, and `107` at `-O2` — the 8th input element appears
where the 1st should be. The reproduction is a `reduced.ll` plus a
twenty-line `main.c`:

```bash
opt reduced.ll -S -o reduced_optimized.ll \
  -passes='inline,loop-rotate,sroa,instcombine,loop-vectorize'
clang -O0 main.c reduced_optimized.ll && ./a.out   # expect 100, got 107
```

The C driver is twenty lines and obviously correct. There is nothing in it to
find. A source indexer has full visibility into the program and still has
nothing to say, because the defect is introduced by a pass pipeline that runs
after the source is gone.

Note where the fix landed: Loop Access Analysis concluded that a load and a
store to the same loop-invariant address were independent, so vectorization
was legal when it was not. That is an IR-level analysis reaching an IR-level
conclusion — the same layer the evidence has to come from.

## Case 2 — there is no indexable C or C++ source at all

**[JuliaLang/julia#62420](https://github.com/JuliaLang/julia/issues/62420)**
— *Wrong results on 1.13.0-rc1 with AVX-512: LLVM 20 VectorCombine
`foldShuffleToIdentity` miscompile*. **Open.** The upstream LLVM fix
(`fd40c60665`) is in `main` but has not been backported to the release
branches Julia builds against.

A small SIMD kernel computing `A ⋅ A'` returns a result whose 5th element is a
bit-identical copy of the 4th. The trigger conditions are all properties of
the compiled artifact rather than of the program text:

- only on machines exposing AVX-512 (Ice Lake, Emerald Rapids, Granite
  Rapids, some EPYC parts);
- only when the kernel is compiled standalone — the `sret [6 x float]` return
  shape is part of the triggering structure, and inlining it into the caller
  makes the defect disappear;
- Julia 1.12 (LLVM 18) is unaffected, because `foldShuffleToIdentity` was
  added in LLVM 19;
- an AVX2-only build passes on the same machine.

Root cause: `VectorCombine::foldShuffleToIdentity` classifies leaf operands by
`Use*`. Two paths reach the same `Use` of one `<2 x float>` load with
different lane patterns — one identity, one splat of lane 1 — but produce the
same classification key. `generateNewInstTree` consults the identity leaves
first, so the splat operand is emitted as a bare load, producing
`fmul <2 x float> %load, %load` instead of a multiply against the splat. The
wrong lane happens to hold the previous output element, which is where the
"5th element duplicates the 4th" signature comes from.

The Julia source is correct. There is no C++ to index — the kernel is Julia
plus `llvmcall`. The same source produces different IR and different results
across target CPUs and LLVM versions. The defect exists only in a specific
artifact, and only IR-level comparison locates it. The issue includes a
twenty-line `.ll` that reproduces it under a single pass:

```bash
opt -passes=vector-combine repro.ll -S    # emits the wrong fmul
```

## Case 3 — no source, and above the layer IREZ covers

**[jax-ml/jax#35409](https://github.com/jax-ml/jax/issues/35409)** —
*[XLA:TPU] Unsafe `aliasing_operands` on `scatter-add` fusion causes
intermittent NaNs*. **Open.**

A tied embedding matrix is updated along two independent paths during
backpropagation: a sparse gather gradient and a dense `dot_general` gradient.
When XLA lets the sparse `scatter-add` fusion reuse the embedding buffer
in place via `aliasing_operands`, the two writes race and corrupt each other,
producing intermittent NaNs. The issue carries an HLO-level trace comparison:
in the failing configuration the independent `add` is gone, and the two
fusions no longer compute separately before merging.

There is no user-authored C++ to index anywhere in this picture.

**IREZ does not cover this case.** The evidence lives in HLO, and IREZ
consumes LLVM IR. It is listed here because it marks the outer edge of the
argument — the class of question generalizes past what this tool ingests. To
use IREZ on an XLA problem at all, you would work from the LLVM IR that XLA
dumps for the CPU/GPU backends, which is a different and narrower question
than the one this issue is about. Do not read this section as a claim of HLO
support.

## Further examples

Same shape, listed for reference rather than developed:

- **[llvm#69744](https://github.com/llvm/llvm-project/issues/69744)** —
  *LoopVectorize Miscompilation with Aliases in clang 15+*. **Closed.**
  An array holds duplicate pointers to one object, so `obj->ref_count++`
  vectorizes across iterations that alias, and the count ends up short.
  Whether two pointers may alias is a conclusion of an IR-level analysis; the
  source shows no ordering violation at all.

- **[llvm#48115](https://github.com/llvm/llvm-project/issues/48115)** —
  *[NVPTX] Miscompilation in trivial fixed-stride loop*. **Open** since
  2021-01-16. A CUDA copy loop over 64 elements with a constant stride of 32
  processes only 32 of them at `-O3`; the emitted PTX compares against
  `i - 32`. `-O0` and a `blockDim.x` stride are both correct. This one sits
  *below* IR, in the backend — a reminder that the artifact chain continues
  past the layer IREZ indexes.

- **[llvm#89112](https://github.com/llvm/llvm-project/issues/89112)** —
  *clang -O0 miscompilation on aarch64-darwin*. **Closed.** Atomic
  compare-and-swap, exchange, and fetch-add return wrong values at `-O0`,
  while `-O1` and above are correct. Optimization passes are not the only
  place a defect can enter; any link in the artifact chain will do.

## What IREZ contributes, and what it does not

For a case like #186922 or #62420, the workflow is:

1. `ingest` the IR from either side of the transformation as two immutable
   artifacts;
2. locate the function, then use bounded queries — `trace-return`, `graph`,
   `uses`, `slice`, `guards` — to compare operand structure across the two;
3. read the provenance recorded with each artifact: which target, which LLVM
   build, which adapter version produced this evidence.

What that gets you is **bounded, cited structural evidence with explicit
limits**: every response distinguishes exact results from partial,
conservative, unknown, and unsupported ones, and reports its own truncation
and boundaries rather than silently returning less.

What it does not get you:

- IREZ does not diff pass pipelines or attribute a change to a pass. It
  indexes the artifacts you give it; choosing and producing the before/after
  pair is your job.
- It does not solve path conditions, resolve indirect calls, or model memory
  dependencies. Those are reported as unknown or unsupported, not guessed.
- It does not observe run-time values. Nothing here replaces running the
  program.
- It answers no question that source-level tooling already answers well. For
  "where is this function called", use clangd.

The claim is narrow on purpose: when the artifact is the thing under
suspicion, you need the artifact as evidence, and you need to know exactly how
far that evidence goes.
