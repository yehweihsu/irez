#!/usr/bin/env python3
"""Generate version/capability contract constants from contract.json.

contract.json is the single source of truth for the IREZ product/schema/
capability contract. During the pre-release edit/compile/verify loop you
change `irez_version` in contract.json ONCE and re-run this script; every
consumer follows:

  - src/contract.h      (generated; consumed by schema.h, service.cpp,
                         store.cpp, adapter.cpp)
  - mcp/src/contract.rs (generated; consumed by mcp/src/install.rs)
  - mcp/Cargo.toml      (version line rewritten; Cargo cannot read JSON)
  - CMakeLists.txt      (reads irez_version from contract.json directly)
  - scripts/package_release.py (reads the schema versions from contract.json)

Usage:
  python scripts/generate_contract.py           # regenerate (idempotent)
  python scripts/generate_contract.py --check   # fail if outputs are stale
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = "// GENERATED FROM contract.json BY scripts/generate_contract.py - DO NOT EDIT.\n"
RUST_HEADER = "//! GENERATED FROM contract.json BY scripts/generate_contract.py - DO NOT EDIT.\n"


def load_contract() -> dict:
    return json.loads((ROOT / "contract.json").read_text(encoding="utf-8"))


def render_cpp(contract: dict) -> str:
    capabilities = "".join(
        f'    {{"{entry["name"]}", "{entry["precision"]}"}},\n'
        for entry in contract["materialization_capabilities"]
    )
    return (
        HEADER
        + "#pragma once\n\n"
        + "#include <cstddef>\n\n"
        + "namespace irez {\n\n"
        + f"inline constexpr int kSchemaVersion = {contract['db_schema_version']};\n"
        + f"inline constexpr int kApiSchemaVersion = {contract['api_schema_version']};\n"
        + f"inline constexpr int kAnalysisSchemaVersion = {contract['analysis_schema_version']};\n"
        + f"inline constexpr int kAdapterVersion = {contract['adapter_version']};\n"
        + f'inline constexpr const char *kAdapterId = "{contract["adapter_id"]}";\n'
        + f'inline constexpr const char *kAdapterVersionString = "{contract["adapter_version"]}";\n\n'
        + "// The exact capability set a fresh materialization must complete.\n"
        + "// The cache-validity check compares this set exactly; a foreign or\n"
        + "// partial manifest with the same count is stale, never current.\n"
        + "struct MaterializationCapability {\n"
        + "  const char *name;\n"
        + "  const char *precision;\n"
        + "};\n"
        + "inline constexpr MaterializationCapability kMaterializationCapabilities[] = {\n"
        + capabilities
        + "};\n"
        + "inline constexpr std::size_t kMaterializationCapabilityCount =\n"
        + "    sizeof(kMaterializationCapabilities) / sizeof(kMaterializationCapabilities[0]);\n\n"
        + "} // namespace irez\n"
    )


def render_rust(contract: dict) -> str:
    return (
        RUST_HEADER
        + f"pub const IREZ_VERSION: &str = \"{contract['irez_version']}\";\n"
        + f"pub const DB_SCHEMA_VERSION: u64 = {contract['db_schema_version']};\n"
        + f"pub const API_SCHEMA_VERSION: u64 = {contract['api_schema_version']};\n"
        + f"pub const ANALYSIS_SCHEMA_VERSION: u64 = {contract['analysis_schema_version']};\n"
        + f"pub const ADAPTER_ID: &str = \"{contract['adapter_id']}\";\n"
        + f"pub const ADAPTER_VERSION: u64 = {contract['adapter_version']};\n"
        + f"pub const CLI_CONTRACT_VERSION: u64 = {contract['cli_contract_version']};\n"
        + f"pub const MCP_CONTRACT_VERSION: u64 = {contract['mcp_contract_version']};\n"
    )


def render_cargo_toml(contract: dict) -> str:
    """Cargo.toml cannot read JSON; rewrite only its [package] version line."""
    cargo_path = ROOT / "mcp" / "Cargo.toml"
    text = cargo_path.read_text(encoding="utf-8")
    updated, count = re.subn(
        r'^version = "[0-9]+\.[0-9]+\.[0-9]+"',
        f'version = "{contract["irez_version"]}"',
        text,
        count=1,
        flags=re.MULTILINE,
    )
    if count != 1:
        raise SystemExit("mcp/Cargo.toml: cannot find a single [package] version line")
    return updated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="verify generated files are up to date")
    args = parser.parse_args()
    contract = load_contract()
    targets = {
        ROOT / "src" / "contract.h": render_cpp(contract),
        ROOT / "mcp" / "src" / "contract.rs": render_rust(contract),
        ROOT / "mcp" / "Cargo.toml": render_cargo_toml(contract),
    }
    problems = []
    if args.check:
        for path, expected in targets.items():
            actual = path.read_text(encoding="utf-8") if path.exists() else None
            if actual != expected:
                problems.append(f"{path.relative_to(ROOT)} is stale; "
                                "run scripts/generate_contract.py")
    else:
        for path, content in targets.items():
            if path.exists() and path.read_text(encoding="utf-8") == content:
                continue
            path.write_text(content, encoding="utf-8", newline="\n")
            print(f"wrote {path.relative_to(ROOT)}")
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
