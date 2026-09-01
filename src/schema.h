#pragma once

// Version numbers and the materialization capability contract are generated
// from contract.json; do not add copies of them here.
#include "contract.h"

namespace irez {

#ifndef IREZ_VERSION
#define IREZ_VERSION "0.1.0"
#endif
inline constexpr const char *kIrezVersion = IREZ_VERSION;

// Git revision (or source snapshot id) of this build, injected by CMake.
// "unknown" means the build tree had no usable VCS information; doctor
// treats that as a warning, never as proof of identity.
#ifndef IREZ_BUILD_REVISION
#define IREZ_BUILD_REVISION "unknown"
#endif
inline constexpr const char *kBuildRevision = IREZ_BUILD_REVISION;

// Identical DDL to IREZ_V00_00 (Python) so both implementations can share a
// state directory during migration and differential testing.
inline constexpr const char *kMigration1 = R"SQL(
CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE investigations (
 id TEXT PRIMARY KEY, name TEXT NOT NULL, created_at TEXT NOT NULL,
 metadata_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE artifacts (
 id TEXT PRIMARY KEY, investigation_id TEXT NOT NULL REFERENCES investigations(id),
 kind TEXT NOT NULL, content_sha256 TEXT NOT NULL, stored_path TEXT NOT NULL,
 original_path TEXT, media_type TEXT NOT NULL, adapter_id TEXT,
 adapter_version TEXT, dialect_version TEXT, target_triple TEXT, data_layout TEXT,
 metadata_json TEXT NOT NULL DEFAULT '{}',
 UNIQUE(investigation_id, content_sha256)
);
CREATE TABLE analysis_runs (
 id TEXT PRIMARY KEY, investigation_id TEXT NOT NULL REFERENCES investigations(id),
 producer TEXT NOT NULL, producer_version TEXT NOT NULL, invocation_json TEXT NOT NULL,
 configuration_json TEXT NOT NULL, input_artifacts_json TEXT NOT NULL,
 started_at TEXT NOT NULL, ended_at TEXT, status TEXT NOT NULL, error_json TEXT
);
CREATE TABLE entities (
 id TEXT PRIMARY KEY, artifact_id TEXT NOT NULL REFERENCES artifacts(id),
 function_id TEXT, block_id TEXT, kind TEXT NOT NULL, local_key TEXT NOT NULL,
 ordinal INTEGER, name TEXT, opcode TEXT, llvm_type TEXT,
 materialization TEXT NOT NULL, exact_text TEXT,
 attributes_json TEXT NOT NULL DEFAULT '{}', UNIQUE(artifact_id, local_key)
);
CREATE TABLE relations (
 id INTEGER PRIMARY KEY, artifact_id TEXT NOT NULL REFERENCES artifacts(id),
 function_id TEXT, src_id TEXT NOT NULL REFERENCES entities(id),
 dst_id TEXT NOT NULL REFERENCES entities(id), kind TEXT NOT NULL, ordinal INTEGER,
 analysis_run_id TEXT REFERENCES analysis_runs(id), modality TEXT NOT NULL,
 precision TEXT NOT NULL, attributes_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE source_locations (
 id INTEGER PRIMARY KEY, artifact_id TEXT NOT NULL REFERENCES artifacts(id),
 entity_id TEXT NOT NULL REFERENCES entities(id), file TEXT NOT NULL, line INTEGER,
 column_no INTEGER, discriminator INTEGER, inline_depth INTEGER NOT NULL,
 scope_name TEXT, analysis_run_id TEXT REFERENCES analysis_runs(id)
);
CREATE TABLE materializations (
 artifact_id TEXT NOT NULL REFERENCES artifacts(id),
 function_id TEXT NOT NULL REFERENCES entities(id), status TEXT NOT NULL,
 analysis_run_id TEXT REFERENCES analysis_runs(id), body_fingerprint TEXT,
 error_json TEXT, PRIMARY KEY (artifact_id, function_id)
);
CREATE INDEX entities_artifact_kind ON entities(artifact_id, kind);
CREATE INDEX entities_function_kind ON entities(function_id, kind);
CREATE INDEX relations_src_kind ON relations(src_id, kind);
CREATE INDEX relations_dst_kind ON relations(dst_id, kind);
CREATE INDEX relations_function_kind ON relations(function_id, kind);
CREATE INDEX source_locations_entity ON source_locations(entity_id, inline_depth);
)SQL";

// V2 makes analysis completion explicit.  A missing relation row is a valid
// exact empty result; it is never used as a proxy for whether an analyzer ran.
inline constexpr const char *kMigration2 = R"SQL(
ALTER TABLE artifacts ADD COLUMN catalog_status TEXT NOT NULL DEFAULT 'stored';
ALTER TABLE artifacts ADD COLUMN catalog_run_id TEXT REFERENCES analysis_runs(id);
ALTER TABLE artifacts ADD COLUMN catalog_error_json TEXT;
ALTER TABLE artifacts ADD COLUMN catalog_claim_owner TEXT;
ALTER TABLE artifacts ADD COLUMN catalog_claim_started_at TEXT;
ALTER TABLE artifacts ADD COLUMN requested_index_level TEXT NOT NULL DEFAULT 'catalog';
ALTER TABLE artifacts ADD COLUMN completed_index_level TEXT NOT NULL DEFAULT 'none';
ALTER TABLE artifacts ADD COLUMN analysis_schema_version INTEGER NOT NULL DEFAULT 2;
ALTER TABLE materializations ADD COLUMN claim_owner TEXT;
ALTER TABLE materializations ADD COLUMN claim_started_at TEXT;
ALTER TABLE materializations ADD COLUMN adapter_id TEXT;
ALTER TABLE materializations ADD COLUMN adapter_version TEXT;
ALTER TABLE materializations ADD COLUMN analysis_schema_version INTEGER;
CREATE TABLE materialization_capabilities (
 function_id TEXT NOT NULL REFERENCES entities(id),
 capability TEXT NOT NULL, status TEXT NOT NULL, precision TEXT,
 analyzer TEXT NOT NULL, analyzer_version TEXT NOT NULL,
 analysis_schema_version INTEGER NOT NULL,
 analysis_run_id TEXT NOT NULL REFERENCES analysis_runs(id),
 completed_at TEXT NOT NULL,
 PRIMARY KEY(function_id, capability)
);
CREATE INDEX materialization_capabilities_run
 ON materialization_capabilities(analysis_run_id);
UPDATE artifacts SET catalog_status = CASE
 WHEN EXISTS(SELECT 1 FROM entities e WHERE e.artifact_id=artifacts.id
             AND e.kind='function') THEN 'catalog_ready' ELSE 'stored' END;
UPDATE artifacts SET completed_index_level = CASE
 WHEN catalog_status='catalog_ready' THEN 'catalog' ELSE 'none' END;
UPDATE materializations SET adapter_id='llvm-ir', adapter_version='1',
 analysis_schema_version=2 WHERE status='structure_ready';
UPDATE schema_meta SET value='2' WHERE key='schema_version';
)SQL";

} // namespace irez
