#!/usr/bin/env python3
"""Shared third-party component metadata for release notices and SPDX output."""

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SQLITECPP_VERSION = "3.3.3"
SQLITE_VERSION = "3.49.2"
ZSTD_FALLBACK_VERSION = "1.5.7"


def validate_native_pins():
    """Keep compliance metadata coupled to the dependency pins in CMake."""
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    expected = (
        f"SQLiteCpp/tar.gz/{SQLITECPP_VERSION}",
        f"zstd-{ZSTD_FALLBACK_VERSION}.tar.gz",
    )
    missing = [value for value in expected if value not in cmake]
    if missing:
        raise RuntimeError(
            "native compliance metadata does not match CMake pins: " + ", ".join(missing))


def release_llvm_versions():
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    versions = set(re.findall(r"^\s+llvm:\s*([0-9]+(?:\.[0-9]+)+)\s*$", workflow, re.MULTILINE))
    if not versions:
        raise RuntimeError("release workflow must pin at least one LLVM version")
    return sorted(versions, key=lambda value: tuple(map(int, value.split("."))))


def normalize_license(expression):
    """Return a valid SPDX expression for legacy Cargo license spellings."""
    if not expression:
        raise RuntimeError("dependency has no declared license")
    return expression.replace("MIT/Apache-2.0", "MIT OR Apache-2.0")


def concluded_license(expression):
    """Record the permissive branch IREZ uses for multi-licensed components."""
    choices = {
        "MIT OR Apache-2.0": "Apache-2.0",
        "Apache-2.0 OR MIT": "Apache-2.0",
        "Unlicense OR MIT": "MIT",
        "MIT OR Apache-2.0 OR LGPL-2.1-or-later": "Apache-2.0",
        "Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT": "Apache-2.0",
        "(MIT OR Apache-2.0) AND Unicode-3.0": "Apache-2.0 AND Unicode-3.0",
    }
    return choices.get(expression, expression)


def cargo_metadata(platform=None, offline=True):
    command = [
        "cargo", "metadata", "--manifest-path", str(ROOT / "mcp/Cargo.toml"),
        "--locked", "--format-version", "1",
    ]
    if platform:
        command.extend(["--filter-platform", platform])
    if offline:
        command.append("--offline")
    # Cargo emits JSON as UTF-8. Be explicit so Windows does not decode author
    # names through the active ANSI code page and make generated notices differ
    # from Linux.
    completed = subprocess.run(
        command, check=True, capture_output=True, text=True, encoding="utf-8")
    return json.loads(completed.stdout)


def package_license_files(package):
    """Read the top-level license/notice documents shipped in a Cargo package."""
    root = Path(package["manifest_path"]).parent
    paths = set()
    explicit = package.get("license_file")
    if explicit:
        paths.add((root / explicit).resolve())
    for path in root.iterdir():
        upper = path.name.upper()
        if path.is_file() and upper.startswith(
                ("LICENSE", "COPYING", "UNLICENSE", "NOTICE", "AUTHORS")):
            paths.add(path.resolve())
    documents = []
    for path in sorted(paths, key=lambda item: item.name.lower()):
        try:
            text = path.read_text(encoding="utf-8").replace("\r\n", "\n").strip()
        except UnicodeDecodeError:
            continue
        if text:
            documents.append({"name": path.name, "text": text})
    return documents


def copyright_text(documents):
    lines = []
    for document in documents:
        for line in document["text"].splitlines():
            stripped = line.strip()
            if re.match(r"^(copyright|©)", stripped, re.IGNORECASE):
                if stripped not in lines:
                    lines.append(stripped)
    return "\n".join(lines) if lines else "NOASSERTION"


def cargo_components(platform=None, offline=True, include_documents=False):
    metadata = cargo_metadata(platform=platform, offline=offline)
    active = {node["id"] for node in metadata["resolve"]["nodes"]}
    result = []
    for package in metadata["packages"]:
        if package["id"] not in active or package["name"] == "irez-mcp":
            continue
        declared = normalize_license(package.get("license"))
        documents = package_license_files(package)
        component = {
            "name": package["name"],
            "version": package["version"],
            "license_declared": declared,
            "license_concluded": concluded_license(declared),
            "download_location": (f"https://crates.io/api/v1/crates/{package['name']}/"
                                  f"{package['version']}/download"),
            "copyright_text": copyright_text(documents),
            "supplier": "NOASSERTION",
            "purl": f"pkg:cargo/{package['name']}@{package['version']}",
            "authors": package.get("authors", []),
        }
        if include_documents:
            component["license_documents"] = documents
        result.append(component)
    return sorted(result, key=lambda item: (item["name"].lower(), item["version"]))


def llvm_component(llvm_version):
    return {
        "name": "LLVM",
        "version": llvm_version,
        "license_declared": "Apache-2.0 WITH LLVM-exception",
        "license_concluded": "Apache-2.0 WITH LLVM-exception",
        "download_location": ("https://github.com/llvm/llvm-project/releases/tag/"
                              f"llvmorg-{llvm_version}"),
        "copyright_text": "Copyright LLVM Project contributors",
        "supplier": "Organization: LLVM Project",
        "purl": f"pkg:github/llvm/llvm-project@llvmorg-{llvm_version}",
    }


def native_components(llvm_version, zstd_version=None):
    validate_native_pins()
    components = [
        llvm_component(llvm_version),
        {
            "name": "SQLiteCpp",
            "version": SQLITECPP_VERSION,
            "license_declared": "MIT",
            "license_concluded": "MIT",
            "download_location": ("https://github.com/SRombauts/SQLiteCpp/archive/refs/tags/"
                                  f"{SQLITECPP_VERSION}.tar.gz"),
            "copyright_text": "Copyright (c) 2012-2025 Sébastien Rombauts",
            "supplier": "Person: Sébastien Rombauts",
            "purl": f"pkg:github/SRombauts/SQLiteCpp@{SQLITECPP_VERSION}",
        },
        {
            "name": "SQLite",
            "version": SQLITE_VERSION,
            "license_declared": "LicenseRef-SQLite-Public-Domain",
            "license_concluded": "LicenseRef-SQLite-Public-Domain",
            "download_location": ("https://github.com/SRombauts/SQLiteCpp/archive/refs/tags/"
                                  f"{SQLITECPP_VERSION}.tar.gz"),
            "copyright_text": "NONE",
            "supplier": "Organization: SQLite",
            "purl": f"pkg:generic/sqlite@{SQLITE_VERSION}",
        },
    ]
    if zstd_version:
        components.append({
            "name": "zstd",
            "version": zstd_version,
            "license_declared": "BSD-3-Clause OR GPL-2.0-only",
            "license_concluded": "BSD-3-Clause",
            "download_location": ("https://github.com/facebook/zstd/releases/tag/"
                                  f"v{zstd_version}"),
            "copyright_text": "Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.",
            "supplier": "Organization: Meta Platforms, Inc.",
            "purl": f"pkg:github/facebook/zstd@v{zstd_version}",
        })
    return components
