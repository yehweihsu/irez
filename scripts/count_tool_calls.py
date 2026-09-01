#!/usr/bin/env python3
"""Count IREZ MCP tool calls from a host session/event log.

The tool-call count of a bounded investigation is an evaluation metric owned
by the benchmark harness; it must be computed from the recorded event log,
never self-reported by the evaluated agent (the fld1 rerun reported 18 calls
when the log contained 20).

Input: a host session export as JSON or JSONL. Host layouts differ, so the
scanner recursively looks for anything that structurally is a tool call:

  - MCP JSON-RPC:  {"method": "tools/call", "params": {"name": ..., "arguments": {...}}}
  - generic logs:  {"name": "irez_*", "arguments": {...}} or {"name": ..., "input": {...}}

Only irez_* tool calls are counted. Output: total calls, per-tool counts,
and groups of exact-duplicate (tool, arguments) calls — the redundancy
metric of the bounded workflow (the rerun had 4 identical trace_return
calls that a projection-aware agent would not have needed).

Usage:
  python scripts/count_tool_calls.py session.jsonl
  python scripts/count_tool_calls.py session.json --json
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path


def iter_documents(text: str):
    """Yield top-level JSON values from a JSON document or JSONL stream."""
    stripped = text.strip()
    if not stripped:
        return
    try:
        yield json.loads(stripped)
        return
    except json.JSONDecodeError:
        pass
    for line in stripped.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            continue


def find_tool_calls(node, calls):
    """Recursively collect (tool_name, canonical_arguments) pairs."""
    if isinstance(node, dict):
        name = None
        arguments = None
        params = node.get("params")
        if node.get("method") == "tools/call" and isinstance(params, dict):
            name = params.get("name")
            arguments = params.get("arguments")
        elif isinstance(node.get("name"), str) and node["name"].startswith("irez_"):
            name = node["name"]
            arguments = node.get("arguments", node.get("input"))
        if isinstance(name, str) and name.startswith("irez_"):
            calls.append((name, json.dumps(arguments, sort_keys=True,
                                           default=str)))
            return  # do not double-count nested duplicates of the same call
        for value in node.values():
            find_tool_calls(value, calls)
    elif isinstance(node, list):
        for item in node:
            find_tool_calls(item, calls)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="host session export (JSON or JSONL)")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    if not args.log.is_file():
        parser.error(f"log does not exist: {args.log}")

    calls = []
    for document in iter_documents(args.log.read_text(encoding="utf-8", errors="replace")):
        find_tool_calls(document, calls)

    per_tool = Counter(name for name, _ in calls)
    per_call = Counter(calls)
    duplicates = [
        {"tool": name, "arguments": canonical, "count": count}
        for (name, canonical), count in sorted(per_call.items())
        if count > 1
    ]
    report = {
        "log": str(args.log),
        "total_calls": len(calls),
        "unique_calls": len(per_call),
        "duplicate_calls": len(calls) - len(per_call),
        "per_tool": dict(sorted(per_tool.items())),
        "duplicates": duplicates,
    }
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"log: {report['log']}")
    print(f"total irez_* tool calls: {report['total_calls']} "
          f"({report['unique_calls']} unique, {report['duplicate_calls']} repeated)")
    for name, count in report["per_tool"].items():
        print(f"  {name}: {count}")
    for duplicate in duplicates:
        print(f"  DUPLICATE x{duplicate['count']}: {duplicate['tool']} "
              f"{duplicate['arguments'][:100]}")
    if not calls:
        print("warning: no irez_* tool calls found; check the export format",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
