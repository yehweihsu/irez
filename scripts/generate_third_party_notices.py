#!/usr/bin/env python3
"""Generate the checked-in third-party notice file from locked dependencies."""

import argparse
import hashlib
from pathlib import Path

from third_party import (ROOT, ZSTD_FALLBACK_VERSION, cargo_components,
                         llvm_component, native_components, release_llvm_versions)


LLVM_EXCEPTION = """---- LLVM Exceptions to the Apache 2.0 License ----

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision
(Section 3), the indemnity provision (Section 9) or other Section of the
License conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software."""

SQLITECPP_MIT = """Copyright (c) 2012-2025 Sébastien Rombauts (sebastien.rombauts@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE."""

SQLITE_PUBLIC_DOMAIN = """All of the code and documentation in SQLite has been
dedicated to the public domain by the authors. Anyone is free to copy, modify,
publish, use, compile, sell, or distribute the original SQLite code, either in
source code form or as a compiled binary, for any purpose, commercial or
non-commercial, and by any means.

Source: https://www.sqlite.org/copyright.html"""

ZSTD_BSD = """BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""

VALUABLE_MIT = """Copyright (c) 2021 Valuable Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE."""


def row(values):
    return "| " + " | ".join(str(value).replace("|", "\\|") for value in values) + " |"


def render():
    cargo = cargo_components(offline=True, include_documents=True)
    llvm_versions = release_llvm_versions()
    native = [llvm_component(version) for version in llvm_versions]
    native.extend(native_components(llvm_versions[-1], ZSTD_FALLBACK_VERSION)[1:])
    lines = [
        "# Third-Party Notices", "",
        "IREZ is licensed under Apache-2.0. This file records software that may",
        "be incorporated into or required by IREZ release binaries; it does not",
        "change the license of IREZ itself. The release SBOM is authoritative for",
        "the exact platform-specific component set.", "",
        "The complete Apache License 2.0 text is in `LICENSE`.", "",
        "## Native components", "",
        row(("Component", "Version", "Declared license", "IREZ selection")),
        row(("---", "---", "---", "---")),
    ]
    for component in native:
        lines.append(row((component["name"], component["version"],
                          component["license_declared"], component["license_concluded"])))
    lines.extend([
        "", "### LLVM exception", "", "````text", LLVM_EXCEPTION, "````", "",
        "### SQLiteCpp MIT license", "", "````text", SQLITECPP_MIT, "````", "",
        "### SQLite public-domain dedication", "", "````text", SQLITE_PUBLIC_DOMAIN,
        "````", "", "### zstd BSD license", "", "````text", ZSTD_BSD, "````", "",
        "## Rust components", "",
        "The following inventory is generated from `mcp/Cargo.lock` and the",
        "corresponding crate manifests. `IREZ selection` records the permissive",
        "branch used when a crate offers a choice.", "",
        row(("Crate", "Version", "Declared license", "IREZ selection", "Authors")),
        row(("---", "---", "---", "---", "---")),
    ])
    for component in cargo:
        authors = "; ".join(component["authors"]) or "Not stated"
        lines.append(row((component["name"], component["version"],
                          component["license_declared"], component["license_concluded"],
                          authors)))

    groups = {}
    for component in cargo:
        for document in component["license_documents"]:
            digest = hashlib.sha256(document["text"].encode()).hexdigest()
            group = groups.setdefault(digest, {"text": document["text"], "uses": []})
            group["uses"].append(
                f"{component['name']} {component['version']} ({document['name']})")
    valuable = next((item for item in cargo if item["name"] == "valuable"), None)
    if valuable and not valuable["license_documents"]:
        digest = hashlib.sha256(VALUABLE_MIT.encode()).hexdigest()
        groups[digest] = {
            "text": VALUABLE_MIT,
            "uses": [f"valuable {valuable['version']} (upstream LICENSE)"],
        }
    lines.extend(["", "## Rust license and notice texts", ""])
    for index, group in enumerate(sorted(groups.values(), key=lambda item: item["uses"][0]), 1):
        lines.extend([
            f"### Document {index}", "",
            "Applies to: " + "; ".join(sorted(group["uses"])), "",
            "````text", group["text"], "````", "",
        ])
    lines.extend([
        "## Maintenance", "",
        "Regenerate this file after changing native dependency pins or",
        "`mcp/Cargo.lock`:", "", "```console",
        "python scripts/generate_third_party_notices.py", "```", "",
    ])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "THIRD_PARTY_NOTICES.md")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render()
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != content:
            raise SystemExit(f"out of date: {args.output}")
        print(f"up to date: {args.output}")
        return
    args.output.write_text(content, encoding="utf-8", newline="\n")
    print(args.output)


if __name__ == "__main__":
    main()
