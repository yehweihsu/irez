# Release process

The canonical repository is <https://github.com/yehweihsu/irez>.

1. Ensure `ci` passes on baseline and latest Linux/Windows lanes.
2. Bump the version in exactly one place: `irez_version` in `contract.json`.
   Then run `python scripts/generate_contract.py` — it regenerates
   `src/contract.h` and `mcp/src/contract.rs` and rewrites the
   `mcp/Cargo.toml` version line. `CMakeLists.txt` reads `irez_version`
   from `contract.json` at configure time, and `package_release.py` refuses
   to bundle when any consumer disagrees. During the pre-release
   edit/compile/verify loop this is the only version edit ever needed;
   schema versions and the materialization capability set live in the same
   file. Add the corresponding user-visible section to `CHANGELOG.md`; the
   release job fails if it cannot extract non-empty notes for that version.
   A prerelease tag may append a SemVer suffix to that product version (for
   example, `0.1.0-rc.1`); do not put the suffix in `contract.json`.
3. Push an annotated `vX.Y.Z` tag, or invoke the release workflow manually
   with a version.
4. The workflow builds on Ubuntu 22.04 and Windows Server 2022, runs the C++,
   Rust, MCP, and bundle checks, then creates:
   - `irez-X.Y.Z-linux-x86_64.tar.gz`;
   - `irez-X.Y.Z-windows-x86_64.zip`;
   - `NOTICE` and `THIRD_PARTY_NOTICES.md` inside each archive;
   - one SPDX 2.3 JSON SBOM per bundle;
   - `SHA256SUMS`.
5. Download the assets and verify `SHA256SUMS`; for public repositories also
   verify GitHub provenance with `gh attestation verify <asset> --repo
   yehweihsu/irez`.

GitHub artifact attestations are automatically enabled for a public
repository. For a private repository whose GitHub plan supports private
attestations, define the repository variable
`ENABLE_PRIVATE_ATTESTATIONS=true`. Without it, the release still produces
checksums and SPDX SBOMs instead of failing on an unavailable entitlement.

Tags containing a prerelease suffix, such as `v0.1.0-rc.1`, are explicitly
published as GitHub prereleases. Both prereleases and final releases use the
matching base-version section from `CHANGELOG.md`, so multiple tags on one
commit do not produce empty generated notes. The bundle manifest keeps the
tag version in `version` and the compiled product version in `irez_version`
and `skill_version`. Rehearse first in a private
repository; after changing visibility to public, publish another RC and verify
its attestation before creating the final tag. Never move a published tag to
a different commit.

Archives use the source commit timestamp (`SOURCE_DATE_EPOCH`), sorted paths,
normalized ownership and permissions, and stable compression metadata. This
makes repeated builds on an equivalent toolchain byte-reproducible; GitHub
provenance records the actual workflow that produced the published files.

`THIRD_PARTY_NOTICES.md` is generated from the locked Rust packages plus the
pinned native dependencies. After changing either set, run
`python scripts/generate_third_party_notices.py`; CI rejects stale notices.
Each bundle manifest carries the exact platform dependency inventory used to
generate its SBOM. The manifest's top-level `license` remains `Apache-2.0`
because it describes IREZ itself, not an aggregate relicense of dependencies.
