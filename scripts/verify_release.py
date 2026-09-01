#!/usr/bin/env python3
"""Fail closed when an IREZ release archive or manifest is malformed."""
import argparse
import hashlib
import json
import tarfile
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def safe_name(name):
    path = PurePosixPath(name.replace("\\", "/"))
    return not path.is_absolute() and ".." not in path.parts

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    if not args.archive.is_file(): parser.error(f"archive does not exist: {args.archive}")
    with tempfile.TemporaryDirectory(prefix="irez-verify-") as temporary:
        root = Path(temporary)
        if args.archive.suffix == ".zip":
            with zipfile.ZipFile(args.archive) as source:
                if not all(safe_name(item.filename) for item in source.infolist()):
                    raise SystemExit("unsafe ZIP member path")
                source.extractall(root)
        else:
            with tarfile.open(args.archive, "r:gz") as source:
                if not all(safe_name(item.name) and not item.issym() and not item.islnk()
                           for item in source.getmembers()):
                    raise SystemExit("unsafe tar member")
                source.extractall(root)
        roots = [path.parent for path in root.rglob("manifest.json")]
        if len(roots) != 1: raise SystemExit("archive must contain exactly one manifest.json")
        bundle = roots[0]
        manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        if manifest.get("format_version") != 1 or manifest.get("license") != "Apache-2.0":
            raise SystemExit("unsupported or incomplete release manifest")
        expected = manifest.get("files")
        if not isinstance(expected, dict) or not expected: raise SystemExit("empty file manifest")
        for relative, expected_hash in expected.items():
            if not safe_name(relative): raise SystemExit(f"unsafe manifest path: {relative}")
            path = bundle / relative
            if not path.is_file(): raise SystemExit(f"missing file: {relative}")
            if digest(path) != expected_hash: raise SystemExit(f"hash mismatch: {relative}")
        suffix = ".exe" if manifest["platform"].startswith("windows") else ""
        components = manifest.get("third_party_components")
        if not isinstance(components, list) or not components:
            raise SystemExit("release manifest has no third-party component inventory")
        for component in components:
            required = ("name", "version", "license_declared", "license_concluded",
                        "download_location", "copyright_text", "supplier")
            if not all(component.get(field) for field in required):
                raise SystemExit("incomplete third-party component entry")
            if (component["license_declared"] == "NOASSERTION"
                    or component["license_concluded"] == "NOASSERTION"):
                raise SystemExit("third-party dependency license is NOASSERTION")
        for name in (f"bin/irez{suffix}", f"bin/irez-mcp{suffix}", "LICENSE", "NOTICE",
                     "THIRD_PARTY_NOTICES.md", "CHANGELOG.md"):
            if not (bundle / name).is_file(): raise SystemExit(f"required file missing: {name}")
        print(f"verified {args.archive.name}: {len(expected)} files, {manifest['platform']}")

if __name__ == "__main__": main()
