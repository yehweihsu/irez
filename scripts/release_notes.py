#!/usr/bin/env python3
"""Extract one version's GitHub release notes from CHANGELOG.md."""

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def product_version():
    contract = json.loads((ROOT / "contract.json").read_text(encoding="utf-8"))
    return contract["irez_version"]


def base_version(version):
    return re.sub(r"-rc(?:\.[0-9]+)?$", "", version.removeprefix("v"))


def extract(version):
    requested = version.removeprefix("v")
    release = base_version(requested)
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    heading = re.compile(
        rf"^## \[{re.escape(release)}\](?:\s+-\s+[^\n]+)?\s*$", re.MULTILINE)
    match = heading.search(changelog)
    if not match:
        raise RuntimeError(f"CHANGELOG.md has no section for {release}")
    following = re.search(
        r"^(?:## |\[[^\]]+\]:\s)", changelog[match.end():], re.MULTILINE)
    end = match.end() + following.start() if following else len(changelog)
    body = changelog[match.end():end].strip()
    if not body:
        raise RuntimeError(f"CHANGELOG.md section for {release} is empty")
    if requested != release:
        body = (f"> This is the `{requested}` pre-release of IREZ {release}.\n\n"
                + body)
    return body + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default=product_version())
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    notes = extract(args.version)
    if args.check:
        print(f"release notes available for {args.version}")
        return
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(notes, encoding="utf-8", newline="\n")
        print(args.output)
    else:
        print(notes, end="")


if __name__ == "__main__":
    main()
