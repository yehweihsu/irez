#!/usr/bin/env python3
"""Golden differential test for the IREZ_V00_01 (C++) CLI.

Runs a fixed command sequence against a fresh state directory, normalizes
volatile values (uuids, timestamps, paths, build revisions), and compares
every response byte-for-byte with the recorded golden in tests/golden/.

Usage:
  python3 scripts/diff_test.py --write-golden   # record C++ outputs as golden
  python3 scripts/diff_test.py                  # compare C++ vs golden

Every diff is a failure: intentional contract changes must be eyeballed via
--write-golden and the golden diff reviewed in git.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import tempfile
from functools import cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOLDEN_DIR = ROOT / "tests" / "golden"
V01_GOLDEN = GOLDEN_DIR / "v01.json"

UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
TS_RE = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+\+00:00")


def sha16(path: Path) -> str:
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def command_sequence() -> list[tuple[str, list[str]]]:
    """(label, argv-suffix) pairs. Handles are deterministic per fixture."""
    nf = sha16(ROOT / "fixtures" / "nonfloating.ll")
    ic = sha16(ROOT / "fixtures" / "internal_call.ll")
    sk = sha16(ROOT / "fixtures" / "store_kernel.ll")
    gl = sha16(ROOT / "fixtures" / "globals.ll")
    fn = f"irez:{nf}:llvm:function:f1"      # choose
    inst_ret = f"irez:{nf}:llvm:inst:f1:7"  # ret in choose
    inst_phi = f"irez:{nf}:llvm:inst:f1:5"
    inst_add = f"irez:{nf}:llvm:inst:f1:1"
    inst_call = f"irez:{nf}:llvm:inst:f1:6"  # external call
    arg_x = f"irez:{nf}:llvm:arg:f1:0"
    ic_caller = f"irez:{ic}:llvm:function:f1"
    ic_helper = f"irez:{ic}:llvm:function:f0"
    ic_call = f"irez:{ic}:llvm:inst:f1:0"    # internal call to helper
    sk_kernel = f"irez:{sk}:llvm:function:f0"  # divide_bitcast_fusion
    return [
        ("init", ["init", "--name", "golden"]),
        ("status-empty", ["status"]),
        ("ingest-nonfloating", ["ingest", "llvm",
                                str(ROOT / "fixtures" / "nonfloating.ll"),
                                "--index", "catalog"]),
        ("ingest-dedup", ["ingest", "llvm",
                          str(ROOT / "fixtures" / "nonfloating.ll"),
                          "--index", "catalog"]),
        ("status", ["status"]),
        ("artifacts", ["artifacts"]),
        ("functions", ["functions"]),
        ("functions-match", ["functions", "--match", "choose"]),
        ("functions-bad-regex", ["functions", "--match", "["]),
        ("capabilities", ["capabilities"]),
        ("materialize-choose", ["materialize", "function", fn]),
        ("materialize-cached", ["materialize", "function", fn]),
        ("show-function", ["show", fn]),
        ("show-function-exact", ["show", fn, "--view", "exact"]),
        ("show-function-returns", ["show", fn, "--view", "children",
                                    "--kind", "return", "--budget-nodes", "5"]),
        ("trace-return", ["trace-return", fn, "--budget-nodes", "20",
                           "--budget-depth", "8"]),
        ("trace-return-graph", ["trace-return", fn, "--budget-nodes", "20",
                                 "--budget-depth", "8", "--detail", "graph"]),
        ("trace-return-summary", ["trace-return", fn, "--budget-nodes", "20",
                                   "--budget-depth", "8", "--detail", "summary"]),
        ("trace-return-include-nodes", ["trace-return", fn, "--budget-nodes", "20",
                                         "--budget-depth", "8", "--detail", "summary",
                                         "--include", "chain,nodes"]),
        ("trace-return-bad-section", ["trace-return", fn, "--include", "bogus"]),
        ("trace-return-flags", ["trace-return", fn, "--include", "chain,flags"]),
        ("ingest-store-kernel", ["ingest", "llvm",
                                 str(ROOT / "fixtures" / "store_kernel.ll"),
                                 "--index", "full"]),
        ("trace-stores-kernel", ["trace-stores", sk_kernel,
                                 "--budget-nodes", "20", "--budget-depth", "8"]),
        ("trace-return-kernel-null", ["trace-return", sk_kernel,
                                      "--budget-nodes", "20",
                                      "--budget-depth", "8"]),
        ("show-ret", ["show", inst_ret]),
        ("source-add", ["source", inst_add]),
        ("uses-arg", ["uses", arg_x]),
        ("slice-default", ["slice", inst_ret]),
        ("slice-operand", ["slice", inst_ret, "--relations", "operand"]),
        ("slice-depth1", ["slice", inst_ret, "--relations", "operand",
                          "--budget-depth", "1"]),
        ("slice-nodes2", ["slice", inst_ret, "--relations", "operand",
                          "--budget-nodes", "2"]),
        ("graph-ret", ["graph", inst_ret]),
        ("graph-exact-ir", ["graph", inst_ret, "--format", "exact-ir"]),
        ("guards-phi", ["guards", inst_phi]),
        ("expand-external", ["expand", inst_call]),
        ("context-ret", ["context", inst_ret]),
        ("ingest-internal-call", ["ingest", "llvm",
                                  str(ROOT / "fixtures" / "internal_call.ll"),
                                  "--index", "catalog"]),
        ("materialize-caller", ["materialize", "function", ic_caller]),
        ("expand-internal-call", ["expand", ic_call]),          # B1 intentional diff
        ("show-helper-after-expand", ["show", ic_helper]),      # B1 intentional diff
        ("slice-declaration", ["slice", f"irez:{nf}:llvm:function:f0"]),
        ("not-found", ["show", "irez:0000000000000000:llvm:inst:f9:9"]),
        # Exit-code contract probes. These intentionally-failing ingests may
        # persist failed-artifact rows, so they stay at the end of the
        # sequence where they cannot perturb earlier golden entries.
        ("ingest-malformed-ir", ["ingest", "llvm",
                                 str(ROOT / "fixtures" / "malformed.ll")]),
        ("ingest-unsupported-txt", ["ingest", "llvm",
                                    str(ROOT / "fixtures" / "not_ir.txt")]),
        # A3: slicing a module-scoped global must report the scope boundary in
        # unknowns — with default relations (which also trigger the CD
        # capability unknown) and with an explicit non-CD selection.
        ("ingest-globals", ["ingest", "llvm",
                            str(ROOT / "fixtures" / "globals.ll"),
                            "--index", "full"]),
        ("slice-global-forward", ["slice", f"irez:{gl}:llvm:global:g0",
                                  "--direction", "forward"]),
        ("slice-global-backward", ["slice", f"irez:{gl}:llvm:global:g0",
                                   "--direction", "backward",
                                   "--relations", "operand,llvm.references-global"]),
    ]


@cache
def path_spellings(path: Path) -> frozenset[str]:
    """Return stable textual aliases for a path, including Windows 8.3 names."""
    candidates = {str(path)}
    try:
        candidates.add(str(path.resolve()))
    except OSError:
        # Normalization must remain useful even if an already-removed
        # temporary path can no longer be resolved.
        pass
    return frozenset(candidate for spelling in candidates
                     for candidate in (spelling, spelling.replace("\\", "/")))


def normalize(value, state_dir: Path | None = None):
    if isinstance(value, dict):
        dynamic_versions = {"dialect_version", "llvm_build_version",
                            "llvm_ir_reader_version"}
        normalized = {
            key: ("<BUILD_REVISION>" if key == "build_revision" and isinstance(val, str)
                  else "<LLVM_VERSION>" if key in dynamic_versions and isinstance(val, str)
                  else "<BODY_FINGERPRINT>" if key == "body_fingerprint" and isinstance(val, str)
                  else normalize(val, state_dir))
            for key, val in sorted(value.items())
        }
        # state_dir_conflict diagnostics depend on what other state
        # directories happen to exist on the recording machine; they are
        # environment facts, not response contract, so they are excluded
        # from the byte-for-byte comparison.
        diags = normalized.get("diagnostics")
        if isinstance(diags, list):
            normalized["diagnostics"] = [
                item for item in diags
                if not (isinstance(item, dict)
                        and item.get("kind") == "state_dir_conflict")
            ]
        return normalized
    if isinstance(value, list):
        return [normalize(item, state_dir) for item in value]
    if isinstance(value, str):
        value = UUID_RE.sub("<UUID>", value)
        value = TS_RE.sub("<TS>", value)
        if state_dir is not None:
            for spelling in path_spellings(state_dir):
                value = value.replace(spelling, "<STATE>")
        # Mask the checkout root so goldens recorded on one machine/OS
        # (e.g. WSL paths like /mnt/d/...) match runs anywhere else.
        for spelling in path_spellings(ROOT):
            value = value.replace(spelling, "<ROOT>")
        # Canonicalize separators in the remainder of masked paths so that
        # Windows ("<ROOT>\\fixtures\\x.ll") and POSIX recordings agree.
        # Diagnostics may prefix a masked path with a tool name, so the path
        # marker is not necessarily at the start of the string.
        if "<ROOT>" in value or "<STATE>" in value:
            value = value.replace("\\", "/")
        return value
    return value


def run_sequence(runner, state_dir: Path) -> dict[str, dict]:
    outputs = {}
    for label, argv in command_sequence():
        proc = runner(state_dir, argv)
        try:
            payload = json.loads(proc.stdout) if proc.stdout.strip() else None
        except json.JSONDecodeError:
            payload = {"<unparsable stdout>": proc.stdout}
        stderr_payload = None
        if proc.stderr.strip():
            try:
                stderr_payload = json.loads(proc.stderr)
            except json.JSONDecodeError:
                stderr_payload = {"<unparsable stderr>": proc.stderr}
        outputs[label] = {
            "exit_code": proc.returncode,
            "stdout": normalize(payload, state_dir),
            "stderr": normalize(stderr_payload, state_dir),
        }
    return outputs


def cpp_runner(binary: Path):
    def run(state_dir: Path, argv: list[str]) -> subprocess.CompletedProcess:
        return subprocess.run([str(binary), "--state-dir", str(state_dir), *argv],
                              capture_output=True, text=True)
    return run


def find_diffs(expected, actual, path="$"):
    """Yield JSON paths where the two normalized payloads diverge."""
    if type(expected) is not type(actual):
        yield f"{path}: type {type(expected).__name__} != {type(actual).__name__} " \
              f"({expected!r:.80} vs {actual!r:.80})"
        return
    if isinstance(expected, dict):
        for key in expected.keys() | actual.keys():
            if key not in expected:
                yield f"{path}.{key}: only in c++ ({actual[key]!r:.80})"
            elif key not in actual:
                yield f"{path}.{key}: only in golden ({expected[key]!r:.80})"
            else:
                yield from find_diffs(expected[key], actual[key], f"{path}.{key}")
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            yield f"{path}: list length {len(expected)} != {len(actual)}"
        for i, (e, a) in enumerate(zip(expected, actual)):
            yield from find_diffs(e, a, f"{path}[{i}]")
    elif expected != actual:
        yield f"{path}: {expected!r:.100} != {actual!r:.100}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-golden", action="store_true")
    parser.add_argument("--binary", default=str(ROOT / "build" / "irez"))
    parser.add_argument("--golden", type=Path, default=V01_GOLDEN)
    args = parser.parse_args()

    if args.write_golden:
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            outputs = run_sequence(cpp_runner(Path(args.binary)), Path(tmp) / "state")
        args.golden.write_text(
            json.dumps(outputs, indent=2, sort_keys=True) + "\n")
        print(f"wrote {args.golden}")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        cpp = run_sequence(cpp_runner(Path(args.binary)), Path(tmp) / "cpp-state")
        reference = normalize(json.loads(args.golden.read_text()))

    failures = 0
    for label in cpp:
        if cpp[label] != reference.get(label):
            differences = list(find_diffs(reference.get(label), cpp[label]))
            failures += 1
            print(f"DIFF {label}")
            for line in differences[:6]:
                print("   ", line)
    if failures:
        print(f"{failures} unexpected difference(s)")
        return 1
    print(f"all {len(cpp)} commands match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
