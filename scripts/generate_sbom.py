#!/usr/bin/env python3
"""Generate a self-contained SPDX 2.3 JSON SBOM for an IREZ bundle."""
import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path

def spdx_id(value):
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]", "-", value)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--namespace-base", default="https://github.com/yehweihsu/irez")
    args = parser.parse_args()
    manifest = json.loads((args.bundle / "manifest.json").read_text(encoding="utf-8"))
    namespace_hash = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    created_epoch = int(manifest.get("source_date_epoch", 315532800))
    created = datetime.fromtimestamp(created_epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    root_id = "SPDXRef-Package-IREZ"
    packages = [{
        "name": "IREZ", "SPDXID": root_id, "versionInfo": manifest["version"],
        "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
        "licenseConcluded": "Apache-2.0", "licenseDeclared": "Apache-2.0",
        "copyrightText": "Copyright 2026 Yewei Xu",
    }]
    dependencies = manifest.get("third_party_components")
    if not isinstance(dependencies, list) or not dependencies:
        raise SystemExit("manifest has no third_party_components")
    relationships = []
    for component in sorted(dependencies, key=lambda item: (item["name"], item["version"])):
        required = ("name", "version", "license_declared", "license_concluded",
                    "download_location", "copyright_text", "supplier")
        if not all(component.get(field) for field in required):
            raise SystemExit("incomplete third-party component entry")
        if (component["license_declared"] == "NOASSERTION"
                or component["license_concluded"] == "NOASSERTION"):
            raise SystemExit("third-party dependency license is NOASSERTION")
        name, version = component["name"], component["version"]
        identifier = spdx_id(f"Package-{name}-{version}")
        package = {
            "name": name, "SPDXID": identifier, "versionInfo": version,
            "downloadLocation": component["download_location"], "filesAnalyzed": False,
            "licenseConcluded": component["license_concluded"],
            "licenseDeclared": component["license_declared"],
            "copyrightText": component["copyright_text"],
            "supplier": component["supplier"],
        }
        if component.get("purl"):
            package["externalRefs"] = [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": component["purl"],
            }]
        packages.append(package)
        relationships.append({"spdxElementId": root_id, "relationshipType": "DEPENDS_ON",
                              "relatedSpdxElement": identifier})
    files = []
    for relative, checksum in sorted(manifest["files"].items()):
        identifier = spdx_id(f"File-{relative}")
        files.append({"fileName": f"./{relative}", "SPDXID": identifier,
                      "checksums": [{"algorithm": "SHA256", "checksumValue": checksum}],
                      "licenseConcluded": "NOASSERTION", "copyrightText": "NOASSERTION"})
        relationships.append({"spdxElementId": root_id, "relationshipType": "CONTAINS",
                              "relatedSpdxElement": identifier})
    document = {
        "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"irez-{manifest['version']}-{manifest['platform']}",
        "documentNamespace": f"{args.namespace_base.rstrip('/')}/sbom/{namespace_hash}",
        "creationInfo": {"created": created, "creators": ["Tool: irez-generate-sbom-1"]},
        "hasExtractedLicensingInfos": [{
            "licenseId": "LicenseRef-SQLite-Public-Domain",
            "name": "SQLite Public Domain Dedication",
            "extractedText": ("All of the code and documentation in SQLite has been "
                              "dedicated to the public domain by the authors."),
            "seeAlsos": ["https://www.sqlite.org/copyright.html"],
        }],
        "packages": packages, "files": files,
        "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                           "relatedSpdxElement": root_id}, *relationships],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(args.output)

if __name__ == "__main__": main()
