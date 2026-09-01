#!/usr/bin/env python3
"""Check that Linux release binaries do not exceed the declared ABI floor."""
import argparse
import re
import subprocess
from pathlib import Path

def versions(binary, namespace):
    output = subprocess.run(["objdump", "-T", binary], check=True,
                            capture_output=True, text=True).stdout
    return [tuple(map(int, match.split(".")))
            for match in re.findall(rf"{namespace}_([0-9]+(?:\.[0-9]+)+)", output)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-glibc", default="2.35")
    parser.add_argument("--require-static-gcc", action="store_true")
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()
    ceiling = tuple(map(int, args.max_glibc.split(".")))
    for binary in args.binaries:
        if not binary.is_file(): parser.error(f"missing binary: {binary}")
        required = versions(binary, "GLIBC")
        maximum = max(required, default=(0,))
        if maximum > ceiling:
            raise SystemExit(f"{binary}: requires GLIBC {'.'.join(map(str, maximum))}, ceiling is {args.max_glibc}")
        linked = subprocess.run(["ldd", binary], check=True, capture_output=True, text=True).stdout
        if args.require_static_gcc and ("libstdc++.so" in linked or "libgcc_s.so" in linked):
            raise SystemExit(f"{binary}: GCC support runtime is still dynamically linked")
        print(f"{binary}: GLIBC <= {'.'.join(map(str, maximum))}")

if __name__ == "__main__": main()
