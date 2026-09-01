#!/usr/bin/env python3
"""Create a versioned IREZ binary release bundle with a hash manifest."""
import argparse
import ctypes
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path

from third_party import (ZLIB_FALLBACK_VERSION, ZSTD_FALLBACK_VERSION,
                         cargo_components, native_components)

ROOT = Path(__file__).resolve().parents[1]

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def archive_epoch():
    # ZIP cannot represent dates before 1980. CI sets SOURCE_DATE_EPOCH to the
    # source commit time; the stable fallback also makes local rebuilds equal.
    return max(315532800, int(os.environ.get("SOURCE_DATE_EPOCH", "315532800")))

def build_revision():
    # IREZ_BUILD_REVISION is authoritative when set (CI/release machinery);
    # otherwise ask git directly. "unknown" is explicit, never silently absent.
    if os.environ.get("IREZ_BUILD_REVISION"):
        return os.environ["IREZ_BUILD_REVISION"]
    probe = subprocess.run(["git", "rev-parse", "--short=12", "HEAD"],
                           cwd=ROOT, capture_output=True, text=True)
    if probe.returncode == 0 and probe.stdout.strip():
        return probe.stdout.strip()
    return "unknown"

def linux_glibc_floor(sources):
    versions = []
    for source in sources:
        symbols = subprocess.run(["objdump", "-T", source], check=True,
                                 capture_output=True, text=True).stdout
        versions.extend(tuple(map(int, value.split(".")))
                        for value in re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", symbols))
    maximum = max(versions, default=(0, 0))
    return ".".join(map(str, maximum))

def linux_zstd_version(sources):
    """Return the linked system zstd version, or None when it is not required."""
    linked = any(
        "libzstd.so" in subprocess.run(
            ["objdump", "-p", source], check=True, capture_output=True, text=True
        ).stdout
        for source in sources
    )
    if not linked:
        return None
    library = ctypes.CDLL("libzstd.so.1")
    library.ZSTD_versionString.restype = ctypes.c_char_p
    return library.ZSTD_versionString().decode("ascii")

def load_contract():
    """The product/schema contract; the single source for the numbers below."""
    return json.loads((ROOT / "contract.json").read_text(encoding="utf-8"))

def declared_versions():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    cargo = (ROOT / "mcp/Cargo.toml").read_text(encoding="utf-8")
    cmake_version = re.search(r"project\(irez VERSION ([0-9]+\.[0-9]+\.[0-9]+)", cmake)
    cargo_version = re.search(r'^version = "([0-9]+\.[0-9]+\.[0-9]+)"', cargo, re.MULTILINE)
    if not cargo_version:
        raise SystemExit("cannot determine Cargo project version")
    if cmake_version:
        # Pinned literal (pre-contract layout): must still agree.
        return cmake_version.group(1), cargo_version.group(1)
    if 'contract.json' not in cmake:
        raise SystemExit("CMakeLists.txt neither pins a version nor reads contract.json")
    contract_version = load_contract()["irez_version"]
    return contract_version, cargo_version.group(1)


def release_version_matches(release_version, product_version):
    """Allow the product version itself or a SemVer-style prerelease of it."""
    pattern = re.compile(
        rf"{re.escape(product_version)}"
        r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    )
    return pattern.fullmatch(release_version) is not None

def write_zip(archive, bundle, name, epoch):
    timestamp = time.gmtime(epoch)[:6]
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in sorted(item for item in bundle.rglob("*") if item.is_file()):
            info = zipfile.ZipInfo(f"{name}/{path.relative_to(bundle).as_posix()}", timestamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if path.parent.name == "bin" else 0o644) << 16
            output.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED,
                            compresslevel=9)

def write_tar_gz(archive, bundle, name, epoch):
    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch,
                           compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as output:
                for path in [bundle, *sorted(bundle.rglob("*"))]:
                    info = output.gettarinfo(path, arcname=(Path(name) / path.relative_to(bundle)).as_posix())
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = epoch
                    if path.is_dir():
                        info.mode = 0o755
                    elif path.parent.name == "bin":
                        info.mode = 0o755
                    else:
                        info.mode = 0o644
                    if path.is_file():
                        with path.open("rb") as stream:
                            output.addfile(info, stream)
                    else:
                        output.addfile(info)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True,
                        choices=["linux-x86_64", "windows-x86_64"])
    parser.add_argument("--version", default=None)
    parser.add_argument("--cpp-bin", type=Path, required=True)
    parser.add_argument("--mcp-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    contract = load_contract()
    if args.version is None:
        args.version = contract["irez_version"]
    cmake_version, cargo_version = declared_versions()
    product_version = contract["irez_version"]
    if cmake_version != product_version or cargo_version != product_version:
        parser.error(f"product version differs between contract {product_version}, "
                     f"CMake {cmake_version}, and Cargo {cargo_version}")
    if not release_version_matches(args.version, product_version):
        parser.error(f"release version {args.version} is neither product version "
                     f"{product_version} nor one of its prereleases")
    epoch = archive_epoch()
    suffix = ".exe" if args.platform.startswith("windows") else ""
    sources = [args.cpp_bin / f"irez{suffix}",
               args.cpp_bin / f"irez-llvm-index{suffix}", args.mcp_bin]
    for source in sources:
        if not source.is_file():
            parser.error(f"missing binary: {source}")
    name = f"irez-{args.version}-{args.platform}"
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="irez-release-") as temporary:
        bundle = Path(temporary) / name
        (bundle / "bin").mkdir(parents=True)
        for source in sources:
            shutil.copy2(source, bundle / "bin" / source.name)
        for item in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md", "README.md",
                     "CHANGELOG.md", "SECURITY.md"):
            shutil.copy2(ROOT / item, bundle / item)
        for item in ("docs", "fixtures"):
            shutil.copytree(ROOT / item, bundle / item,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        files = sorted(path for path in bundle.rglob("*") if path.is_file())
        version_run = subprocess.run([sources[1], "version"], check=True,
                                     capture_output=True, text=True)
        adapter_version = json.loads(version_run.stdout)
        llvm_version = adapter_version.get("llvm_build_version", "unknown")
        cargo_target = ("x86_64-pc-windows-msvc" if args.platform.startswith("windows")
                        else "x86_64-unknown-linux-gnu")
        zstd_version = (ZSTD_FALLBACK_VERSION if args.platform.startswith("windows")
                        else linux_zstd_version(sources))
        zlib_version = (ZLIB_FALLBACK_VERSION
                        if args.platform.startswith("windows") else None)
        components = [
            *native_components(llvm_version, zstd_version, zlib_version),
            *cargo_components(platform=cargo_target, offline=True),
        ]
        minimum_os = ("Windows 10 x86-64" if args.platform.startswith("windows")
                      else f"glibc {linux_glibc_floor(sources)} x86-64")
        manifest = {
            "format_version": 1, "product": "irez", "version": args.version,
            "platform": args.platform, "license": "Apache-2.0",
            "irez_version": product_version,
            "db_schema_version": contract["db_schema_version"],
            "api_schema_version": contract["api_schema_version"],
            "analysis_schema_version": contract["analysis_schema_version"],
            # Pre-release compatibility aliases.
            "state_schema_version": contract["db_schema_version"],
            "cli_contract_version": contract["cli_contract_version"],
            "mcp_contract_version": contract["mcp_contract_version"],
            "skill_version": product_version,
            "llvm_build_version": llvm_version,
            "adapter_version": adapter_version.get("adapter_version", "unknown"),
            "build_revision": build_revision(),
            "minimum_os": minimum_os,
            "source_date_epoch": epoch,
            "third_party_components": components,
            "files": {str(path.relative_to(bundle)).replace(os.sep, "/"): sha256(path)
                      for path in files},
        }
        (bundle / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if args.platform.startswith("windows"):
            archive = args.output / f"{name}.zip"
            write_zip(archive, bundle, name, epoch)
        else:
            archive = args.output / f"{name}.tar.gz"
            write_tar_gz(archive, bundle, name, epoch)
    print(archive)

if __name__ == "__main__":
    main()
