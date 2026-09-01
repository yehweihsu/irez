#include "service.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <system_error>

#include <SQLiteCpp/Transaction.h>

#include "adapter.h"
#include "envelope.h"
#include "error.h"
#include "schema.h"
#include "util.h"
#include "llvm/Config/llvm-config.h"

namespace irez {
namespace {

const std::map<std::string, std::string> kRelationAliases = {
    {"operand", "llvm.operand"},
    {"control", "llvm.control-dependence"},
    {"call", "llvm.calls"},
    {"cfg", "llvm.cfg-successor"},
};

std::optional<std::string> opt_string(const Row &row, const std::string &key) {
  auto it = row.find(key);
  if (it == row.end() || it->second.is_null())
    return std::nullopt;
  return it->second.as_string();
}

// The materialization cache is current only when the completed capability
// manifest is exactly the contract set of this build. Comparing a bare count
// (the pre-contract ">= 6" check) accepted foreign or partial manifests that
// happened to have six rows.
bool capability_manifest_current(SQLite::Database &db,
                                 const std::string &function_id) {
  const auto rows = query_all(
      db, "SELECT capability,precision FROM materialization_capabilities "
          "WHERE function_id=? AND status='completed'",
      {function_id});
  if (rows.size() != kMaterializationCapabilityCount)
    return false;
  for (const auto &expected : kMaterializationCapabilities) {
    const bool found = std::any_of(rows.begin(), rows.end(), [&](const Row &row) {
      return row.at("capability").as_string() == expected.name &&
             opt_string(row, "precision").value_or("") == expected.precision;
    });
    if (!found)
      return false;
  }
  return true;
}

// Decode attributes_json into "attributes", matching Python's _entity_json.
llvm::json::Object entity_json(const Row &row) {
  llvm::json::Object out;
  for (const auto &[name, cell] : row) {
    if (name == "attributes_json") {
      if (!cell.is_null())
        out["attributes"] = parse_json(cell.as_string(), "attributes_json");
      else
        out["attributes"] = llvm::json::Object{};
    } else {
      out[name] = cell_to_json(cell);
    }
  }
  if (const auto it = row.find("id"); it != row.end() && !it->second.is_null())
    out["handle"] = it->second.as_string();
  return out;
}

llvm::json::Object relation_json(const Row &row) {
  // Matches Python's show(): relation rows keep attributes_json as raw text.
  return row_to_json(row);
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

// Adapter-record fields that are ABSENT must become SQL NULL, not "" or 0
// (Python used record.get(...), which yields None). Only present-but-empty
// values may be stored as empty strings.
Param json_string_param(const llvm::json::Object &record, llvm::StringRef key) {
  const llvm::json::Value *value = record.get(key);
  if (!value || value->getAsNull())
    return Param(nullptr);
  if (auto text = record.getString(key))
    return Param(text->str());
  return Param(json_to_string(*value));
}

Param json_int_param(const llvm::json::Object &record, llvm::StringRef key) {
  const llvm::json::Value *value = record.get(key);
  if (!value || value->getAsNull())
    return Param(nullptr);
  if (auto integer = record.getInteger(key))
    return Param(*integer);
  return Param(nullptr);
}

} // namespace

std::string entity_id(const Row &artifact, const std::string &local_key) {
  const std::string short_hash = artifact.at("content_sha256").as_string().substr(0, 16);
  const std::size_t colon = local_key.find(':');
  const std::string head = local_key.substr(0, colon);
  const std::string rest = colon == std::string::npos ? "" : local_key.substr(colon + 1);
  static const std::map<std::string, std::string> kinds = {
      {"function", "function"}, {"block", "block"}, {"inst", "inst"},
      {"arg", "arg"},           {"constant", "constant"}, {"global", "global"},
  };
  const auto it = kinds.find(head);
  const std::string kind = it == kinds.end() ? head : it->second;
  return "irez:" + short_hash + ":llvm:" + kind + ":" + rest;
}

llvm::json::Array evidence_refs(const std::string &artifact_id,
                                const std::vector<std::optional<std::string>> &run_ids) {
  llvm::json::Array out;
  out.push_back(artifact_id);
  std::set<std::string> runs;
  for (const auto &run_id : run_ids)
    if (run_id && !run_id->empty())
      runs.insert(*run_id);
  for (const std::string &run : runs)
    out.push_back(run);
  return out;
}

llvm::json::Array relation_capabilities(const std::vector<std::string> &kinds) {
  static const std::map<std::string, llvm::json::Value> mapping = {
      {"llvm.operand",
       llvm::json::Object{{"name", "operand_graph"}, {"status", "supported"}, {"precision", "exact"}}},
      {"llvm.cfg-successor",
       llvm::json::Object{{"name", "cfg"}, {"status", "supported"}, {"precision", "exact"}}},
      {"llvm.calls",
       llvm::json::Object{{"name", "direct_calls"}, {"status", "supported"}, {"precision", "exact"}}},
      {"llvm.contains",
       llvm::json::Object{{"name", "entity_index"}, {"status", "supported"}, {"precision", "exact"}}},
      {"llvm.references-global",
       llvm::json::Object{{"name", "entity_index"}, {"status", "supported"}, {"precision", "exact"}}},
      {"llvm.control-dependence",
       llvm::json::Object{{"name", "control_dependence"}, {"status", "supported"}, {"precision", "exact"}}},
  };
  llvm::json::Array out;
  for (const std::string &kind : sorted_unique(kinds)) {
    auto it = mapping.find(kind);
    if (it != mapping.end())
      out.push_back(it->second);
  }
  return out;
}

llvm::json::Array flatten_capabilities(const llvm::json::Object &capabilities_record) {
  llvm::json::Array out;
  const llvm::json::Object *inner = capabilities_record.getObject("capabilities");
  if (!inner)
    return out;
  std::vector<std::string> names;
  for (const auto &kv : *inner)
    names.push_back(kv.first.str());
  std::sort(names.begin(), names.end());
  for (const std::string &name : names) {
    const llvm::json::Object *entry = inner->getObject(name);
    if (!entry)
      continue;
    llvm::json::Object item;
    item["name"] = name;
    if (auto status = entry->getString("status"))
      item["status"] = status->str();
    if (auto precision = entry->getString("precision"))
      item["precision"] = precision->str();
    out.push_back(std::move(item));
  }
  return out;
}

Service::Service(std::filesystem::path state_dir) : store_(std::move(state_dir)) {}

std::string Service::resolved_state_dir() const {
  return std::filesystem::weakly_canonical(
             std::filesystem::absolute(store_.state_dir_path()))
      .generic_string();
}

std::string Service::investigation_id(SQLite::Database &db) {
  auto inv = store_.investigation(db);
  if (!inv)
    throw IrezError("investigation is missing", 5);
  return (*inv)["id"].as_string();
}

Row Service::artifact_or_throw(SQLite::Database &db,
                               const std::optional<std::string> &artifact_id) {
  std::optional<Row> row;
  if (artifact_id)
    row = query_one(db, "SELECT * FROM artifacts WHERE id=?", {*artifact_id});
  else
    row = query_one(db, "SELECT * FROM artifacts ORDER BY rowid DESC LIMIT 1");
  if (!row)
    throw NotFound("artifact not found");
  return std::move(*row);
}

void Service::ensure_materialized(SQLite::Database &db, const std::string &handle) {
  auto entity = query_one(db, "SELECT id,kind,function_id,materialization FROM entities WHERE id=?",
                          {handle});
  if (!entity)
    return;
  std::optional<std::string> function = opt_string(*entity, "function_id");
  if ((*entity)["kind"].as_string() == "function")
    function = handle;
  if (!function)
    return;
  auto state = query_one(db, "SELECT * FROM materializations WHERE function_id=?",
                         {*function});
  if (state && (*state)["status"].as_string() == "declaration_only")
    return;
  const bool current = state && (*state)["status"].as_string() == "structure_ready" &&
      opt_string(*state, "adapter_id").value_or("") == kAdapterId &&
      opt_string(*state, "adapter_version").value_or("") == kAdapterVersionString &&
      !(*state)["analysis_schema_version"].is_null() &&
      (*state)["analysis_schema_version"].as_int() == kAnalysisSchemaVersion &&
      capability_manifest_current(db, *function);
  if (!current)
    materialize(*function, true);
}

llvm::json::Object Service::init(const std::string &name) {
  const std::string inv = store_.initialize(name);
  Envelope env;
  env.command = "init";
  env.investigation = inv;
  env.result = llvm::json::Object{{"name", name},
                                  {"state_dir", resolved_state_dir()}};
  return env.build();
}

llvm::json::Object Service::ingest(const std::filesystem::path &path,
                                   const std::string &index, bool refresh) {
  if (index != "catalog" && index != "full")
    throw IrezError("--index must be catalog or full", 2);
  const std::string suffix = lower_suffix(path.string());
  if (suffix != ".ll" && suffix != ".bc")
    throw Unsupported("only .ll and .bc artifacts are supported");

  if (!std::filesystem::exists(store_.db_path()))
    throw IrezError("state is not initialized", 5);

  Row artifact;
  bool created = false;
  {
    SQLite::Database db = store_.connect();
    SQLite::Transaction transaction(db, SQLite::TransactionBehavior::IMMEDIATE);
    auto ingested = store_.ingest_file(db, path);
    transaction.commit();
    artifact = std::move(ingested.first);
    created = ingested.second;
  }
  const std::string artifact_id = artifact["id"].as_string();
  SQLite::Database db = store_.connect();
  artifact = *query_one(db, "SELECT * FROM artifacts WHERE id=?", {artifact_id});
  if (index == "full")
    exec(db, "UPDATE artifacts SET requested_index_level='full' WHERE id=?",
         {artifact_id});
  const bool catalog_current =
      artifact["catalog_status"].as_string() == "catalog_ready" &&
      opt_string(artifact, "adapter_id").value_or("") == kAdapterId &&
      opt_string(artifact, "adapter_version").value_or("") == kAdapterVersionString &&
      artifact["analysis_schema_version"].as_int() == kAnalysisSchemaVersion;
  const bool catalog_refresh = created || refresh || !catalog_current;

  std::string run;
  const std::string claim = uuid4();
  if (catalog_refresh) {
    SQLite::Transaction claim_tx(db);
    exec(db,
         "UPDATE artifacts SET catalog_status='cataloging',catalog_claim_owner=?,"
         "catalog_claim_started_at=?,catalog_error_json=NULL,requested_index_level=? "
         "WHERE id=? AND (catalog_status!='cataloging' OR "
         "julianday(catalog_claim_started_at)<julianday('now','-10 minutes'))",
         {claim, now_iso8601(), index, artifact_id});
    const auto changed = query_one(db, "SELECT changes() AS n");
    if (!changed || changed->at("n").as_int() != 1)
      throw Busy("artifact catalog is being refreshed by another process");
    run = store_.run_start(db, "irez-llvm-index", artifact_id,
                           {"catalog", "--input", artifact["stored_path"].as_string()});
    exec(db, "UPDATE artifacts SET catalog_run_id=? WHERE id=?", {run, artifact_id});
    claim_tx.commit();
  }

  const std::string stored =
      (store_.state_dir_path() / artifact["stored_path"].as_string()).string();
  adapter::Result result;
  if (catalog_refresh) {
    try {
      result = adapter::catalog(stored);
    } catch (const std::exception &error) {
      SQLite::Transaction failed(db);
      store_.run_end(db, run, "failed",
                     llvm::json::Object{{"message", error.what()}});
      exec(db,
           "UPDATE artifacts SET catalog_status='failed',catalog_error_json=?,"
           "catalog_claim_owner=NULL,catalog_claim_started_at=NULL WHERE id=?",
           {json_to_string(llvm::json::Object{{"message", error.what()}}),
            artifact_id});
      failed.commit();
      throw;
    }
    for (const auto &record : result.records) {
      if (record.getString("record") != "error")
        continue;
      const std::string message =
          record.getString("message").value_or("adapter error").str();
      SQLite::Transaction failed(db);
      store_.run_end(db, run, "failed", llvm::json::Object{{"message", message}});
      exec(db,
           "UPDATE artifacts SET catalog_status='failed',catalog_error_json=?,"
           "catalog_claim_owner=NULL,catalog_claim_started_at=NULL WHERE id=?",
           {json_to_string(llvm::json::Object{{"message", message}}), artifact_id});
      failed.commit();
      throw AdapterFailure(message);
    }
  }

  const llvm::json::Object *capabilities_record = nullptr;
  const llvm::json::Object *module_record = nullptr;
  std::int64_t function_count = 0;
  for (const auto &record : result.records) {
    const auto kind = record.getString("record");
    if (kind == "capabilities")
      capabilities_record = &record;
    else if (kind == "module")
      module_record = &record;
    else if (kind == "function")
      ++function_count;
  }
  if (catalog_refresh && !module_record) {
    const std::string message = "adapter emitted no module record";
    SQLite::Transaction failed(db);
    store_.run_end(db, run, "failed", llvm::json::Object{{"message", message}});
    exec(db,
         "UPDATE artifacts SET catalog_status='failed',catalog_error_json=?,"
         "catalog_claim_owner=NULL,catalog_claim_started_at=NULL WHERE id=?",
         {json_to_string(llvm::json::Object{{"message", message}}), artifact_id});
    failed.commit();
    throw AdapterFailure(message);
  }
  const std::string inv = investigation_id(db);
  if (catalog_refresh) try {
    SQLite::Transaction transaction(db);
    llvm::json::Object metadata;
    metadata["module"] = own_json(llvm::json::Value(llvm::json::Object(*module_record)));
    metadata["capabilities"] =
        capabilities_record
            ? own_json(llvm::json::Value(llvm::json::Object(*capabilities_record)))
            : llvm::json::Value(llvm::json::Object{});
    exec(db,
         "UPDATE artifacts SET target_triple=?,data_layout=?,metadata_json=?,"
         "adapter_id=?,adapter_version=?,analysis_schema_version=?,"
         "catalog_status='catalog_ready',catalog_claim_owner=NULL,"
         "catalog_claim_started_at=NULL,completed_index_level='catalog' WHERE id=?",
         {module_record->getString("target_triple").value_or("").str(),
          module_record->getString("data_layout").value_or("").str(),
          json_to_string(llvm::json::Value(std::move(metadata))),
          kAdapterId, kAdapterVersionString,
          std::to_string(kAnalysisSchemaVersion), artifact_id});
    for (const auto &record : result.records) {
      if (record.getString("record") != "function")
        continue;
      const std::string key = record.getString("key").value_or("").str();
      const std::string local = "function:" + key;
      const std::string id = entity_id(artifact, local);
      const bool declaration = record.getBoolean("declaration").value_or(false);
      llvm::json::Object attributes;
      attributes["declaration"] = declaration;
      attributes["linkage"] = record.getString("linkage").value_or("").str();
      attributes["body_fingerprint"] =
          record.getString("body_fingerprint").value_or("").str();
      exec(db,
           "INSERT INTO entities "
           "(id,artifact_id,kind,local_key,ordinal,name,llvm_type,materialization,"
           " attributes_json) VALUES(?,?,?,?,?,?,?,?,?) "
           "ON CONFLICT(artifact_id,local_key) DO UPDATE SET "
           " name=excluded.name,llvm_type=excluded.llvm_type,"
           " attributes_json=excluded.attributes_json",
           {id, artifact_id, "function", local,
            record.getInteger("ordinal").value_or(0),
            record.getString("name").value_or("").str(),
            record.getString("signature").value_or("").str(), "catalog_only",
            json_to_string(llvm::json::Value(std::move(attributes)))});
      // Declarations have no body to materialize; mark them explicitly
      // instead of faking structure_ready (V00_00 bug B7).
      exec(db,
           "INSERT INTO materializations (artifact_id,function_id,status,"
           "body_fingerprint) VALUES(?,?,?,?) ON CONFLICT DO UPDATE SET "
           "body_fingerprint=excluded.body_fingerprint,status=CASE WHEN ? THEN "
           "excluded.status ELSE materializations.status END,claim_owner=NULL,"
           "claim_started_at=NULL",
           {artifact_id, id, declaration ? "declaration_only" : "catalog_only",
            record.getString("body_fingerprint").value_or("").str(), "1"});
      exec(db, "UPDATE entities SET materialization=? WHERE id=?",
           {declaration ? "declaration_only" : "catalog_only", id});
    }
    store_.run_end(db, run, "success");
    transaction.commit();
  } catch (const std::exception &exc) {
    SQLite::Transaction failure(db);
    store_.run_end(db, run, "failed",
                   llvm::json::Object{{"message", exc.what()}});
    exec(db,
         "UPDATE artifacts SET catalog_status='failed',catalog_error_json=?,"
         "catalog_claim_owner=NULL,catalog_claim_started_at=NULL WHERE id=?",
         {json_to_string(llvm::json::Object{{"message", exc.what()}}), artifact_id});
    failure.commit();
    throw;
  }

  if (index == "full") {
    std::vector<std::string> handles;
    for (const auto &row : query_all(
             db,
             "SELECT id,attributes_json FROM entities WHERE artifact_id=? AND "
             "kind='function'",
             {artifact_id})) {
      const auto attributes =
          parse_json(row.at("attributes_json").as_string(), "attributes_json");
      const auto *object = attributes.getAsObject();
      if (!object || !object->getBoolean("declaration").value_or(false))
        handles.push_back(row.at("id").as_string());
    }
    for (const std::string &handle : handles)
      materialize(handle);
    exec(db, "UPDATE artifacts SET completed_index_level='full' WHERE id=?",
         {artifact_id});
  }

  const std::int64_t stored_function_count =
      (*query_one(db, "SELECT count(*) AS n FROM entities WHERE artifact_id=? AND "
                     "kind='function'", {artifact_id}))["n"].as_int();
  artifact = *query_one(db, "SELECT * FROM artifacts WHERE id=?", {artifact_id});
  const auto stored_metadata =
      parse_json(artifact["metadata_json"].as_string(), "artifact metadata_json");
  llvm::json::Value stored_capabilities = llvm::json::Array{};
  if (const auto *object = stored_metadata.getAsObject())
    if (const auto *caps = object->getObject("capabilities"))
      stored_capabilities = flatten_capabilities(*caps);
  Envelope env;
  env.command = "ingest";
  env.investigation = inv;
  env.target = artifact_id;
  // Echo the resolved state directory (backlog F1): the CLI and installed MCP
  // hosts default to different state locations, and a silent split-brain
  // ingest looked like "artifact ingested but never visible". The absolute
  // path in every ingest response makes the actual target library visible.
  env.result = llvm::json::Object{{"artifact", artifact_id},
                                  {"deduplicated", !created},
                                  {"index", index},
                                  {"function_count", stored_function_count},
                                  {"refreshed", catalog_refresh},
                                  {"state_dir", resolved_state_dir()}};
  env.capabilities = std::move(stored_capabilities);
  env.evidence = llvm::json::Array{artifact_id};
  return env.build();
}

llvm::json::Object Service::reindex(const std::string &artifact_handle,
                                    const std::string &index) {
  SQLite::Database db = store_.connect();
  const Row artifact = artifact_or_throw(db, artifact_handle);
  const std::filesystem::path stored =
      store_.state_dir_path() / artifact.at("stored_path").as_string();
  llvm::json::Object response = ingest(stored, index, true);
  response["command"] = "reindex";
  return response;
}

llvm::json::Object Service::status() {
  SQLite::Database db = store_.connect();
  auto version = query_one(db, "SELECT value FROM schema_meta WHERE key='schema_version'");
  if (!version)
    throw IrezError("state is not initialized", 5);
  llvm::json::Object result;
  // Self-describing state: report the resolved state directory so a wrong
  // --state-dir / IREZ_STATE_DIR is visible in the first response instead of
  // surfacing as a confusingly empty or stale artifact list. Forward slashes
  // keep the field byte-identical across OSes.
  const std::string resolved = resolved_state_dir();
  result["state_dir"] = resolved;
  result["schema_version"] = std::stoll((*version)["value"].as_string());
  result["irez_version"] = kIrezVersion;
  result["build_revision"] = kBuildRevision;
  result["db_schema_version"] = kSchemaVersion;
  result["api_schema_version"] = kApiSchemaVersion;
  result["analysis_schema_version"] = kAnalysisSchemaVersion;
  result["adapter_version"] = kAdapterVersion;
  result["llvm_build_version"] = LLVM_VERSION_STRING;
  result["artifacts"] =
      (*query_one(db, "SELECT count(*) AS n FROM artifacts"))["n"].as_int();
  result["functions"] =
      (*query_one(db, "SELECT count(*) AS n FROM entities WHERE kind='function'"))["n"]
          .as_int();
  result["materialized_functions"] =
      (*query_one(
           db, "SELECT count(*) AS n FROM materializations WHERE status='structure_ready'"))
          ["n"]
          .as_int();
  result["materializing_functions"] =
      (*query_one(db, "SELECT count(*) AS n FROM materializations WHERE "
                     "status='materializing'"))["n"].as_int();
  result["failed_materializations"] =
      (*query_one(db, "SELECT count(*) AS n FROM materializations WHERE "
                     "status='failed'"))["n"].as_int();
  result["failed_catalogs"] =
      (*query_one(db, "SELECT count(*) AS n FROM artifacts WHERE "
                     "catalog_status='failed'"))["n"].as_int();
  Envelope env;
  env.command = "status";
  env.investigation = investigation_id(db);
  env.result = std::move(result);

  llvm::json::Array diagnostics;
  // F1: the CLI defaults to ./.irez (or IREZ_STATE_DIR) while installed MCP
  // hosts point at a per-user directory (%LOCALAPPDATA%\irez\<host> or
  // ~/.local/share/irez/<host>). Split-brain state looked like "ingested but
  // never visible". Warn when any other well-known location holds a database.
  {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(".irez");
    auto add_host_dirs = [&candidates](const std::filesystem::path &base) {
      std::error_code ec;
      for (const auto &entry : std::filesystem::directory_iterator(base, ec))
        if (entry.is_directory(ec))
          candidates.push_back(entry.path());
    };
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA"))
      add_host_dirs(std::filesystem::path(local) / "irez");
#else
    if (const char *xdg = std::getenv("XDG_DATA_HOME"))
      add_host_dirs(std::filesystem::path(xdg) / "irez");
    else if (const char *home = std::getenv("HOME"))
      add_host_dirs(std::filesystem::path(home) / ".local" / "share" / "irez");
#endif
    for (const auto &candidate : candidates) {
      std::error_code ec;
      if (!std::filesystem::exists(candidate / "investigation.sqlite", ec) || ec)
        continue;
      const std::string other = std::filesystem::weakly_canonical(
                                    std::filesystem::absolute(candidate), ec)
                                    .generic_string();
      if (ec || other == resolved)
        continue;
      diagnostics.push_back(llvm::json::Object{
          {"kind", "state_dir_conflict"},
          {"status", "warning"},
          {"other_state_dir", other},
          {"message", "another state directory contains an investigation "
                      "database; the CLI and installed MCP hosts default to "
                      "different locations, so pass --state-dir explicitly or "
                      "set IREZ_STATE_DIR to keep both on the same library"}});
    }
  }
  // F6: the materialized_functions count is a storage-level figure; queries
  // materialize on demand, which makes "which functions are queryable" a
  // per-function question. State the semantics instead of letting the count
  // imply more than it measures.
  diagnostics.push_back(llvm::json::Object{
      {"kind", "materialization_counts"},
      {"materialized_functions",
       "counts materializations rows with status='structure_ready'; "
       "query-triggered on-demand materialization is included once the "
       "query completes"},
      {"per_function_state",
       "run functions [--artifact ID] for per-function materialization "
       "status"}});
  env.diagnostics = std::move(diagnostics);
  return env.build();
}

llvm::json::Object Service::backup(const std::filesystem::path &destination) {
  store_.backup(destination);
  llvm::json::Object result;
  result["backup"] = std::filesystem::absolute(destination).string();
  result["status"] = "complete";
  return result;
}

llvm::json::Object Service::artifacts() {
  SQLite::Database db = store_.connect();
  llvm::json::Array rows;
  for (const auto &row : query_all(
           db, "SELECT id,kind,content_sha256,original_path,media_type,target_triple,"
               "adapter_id,adapter_version,dialect_version,catalog_status,"
               "requested_index_level,completed_index_level,analysis_schema_version "
               "FROM artifacts"))
    rows.push_back(row_to_json(row));
  Envelope env;
  env.command = "artifacts";
  env.investigation = investigation_id(db);
  env.result = std::move(rows);
  return env.build();
}

llvm::json::Object Service::functions(const std::optional<std::string> &artifact,
                                      const std::optional<std::string> &match) {
  SQLite::Database db = store_.connect();
  const Row art = artifact_or_throw(db, artifact);
  llvm::json::Array rows;
  std::optional<std::regex> pattern;
  if (match) {
    try {
      pattern.emplace(*match);
    } catch (const std::regex_error &) {
      // std::regex_error::what() is standard-library specific; the response
      // contract needs one stable message across libstdc++/MSVC/libc++.
      throw IrezError(
          "invalid match pattern: not a valid ECMAScript regular expression", 5);
    }
  }
  for (const auto &row : query_all(
           db,
           "SELECT e.id,e.name,e.llvm_type,e.ordinal,m.status,e.attributes_json "
           "FROM entities e JOIN materializations m ON m.function_id=e.id "
           "WHERE e.artifact_id=? AND e.kind='function' ORDER BY e.ordinal",
           {art.at("id").as_string()})) {
    const std::string name = opt_string(row, "name").value_or("");
    if (pattern && !std::regex_search(name, *pattern))
      continue;
    llvm::json::Object item;
    item["id"] = row.at("id").as_string();
    item["handle"] = row.at("id").as_string();
    item["name"] = cell_to_json(row.at("name"));
    item["llvm_type"] = cell_to_json(row.at("llvm_type"));
    item["ordinal"] = cell_to_json(row.at("ordinal"));
    item["status"] = cell_to_json(row.at("status"));
    item["attributes"] =
        parse_json(row.at("attributes_json").as_string(), "attributes_json");
    rows.push_back(std::move(item));
  }
  Envelope env;
  env.command = "functions";
  env.investigation = investigation_id(db);
  env.target = art.at("id").as_string();
  env.result = std::move(rows);
  // F4: with no explicit --artifact the query is silently scoped to the most
  // recently ingested artifact, so a real function in another artifact looked
  // like "not indexed". When an implicitly-scoped query comes back empty, say
  // what scope was applied and how to widen it.
  if (!artifact && env.result.getAsArray()->empty())
    env.diagnostics = llvm::json::Array{llvm::json::Object{
        {"kind", "implicit_target"},
        {"status", "note"},
        {"message", "no artifact was specified; the query ran against the most "
                    "recently ingested artifact only"},
        {"hint", "pass --artifact <id> (see artifacts) to search a specific "
                 "artifact"}}};
  return env.build();
}

llvm::json::Object Service::materialize(const std::string &function_handle,
                                        bool refresh) {
  {
    SQLite::Database db = store_.connect();
    auto function = query_one(
        db, "SELECT * FROM entities WHERE id=? AND kind='function'", {function_handle});
    if (!function)
      throw NotFound("function not found");
    Row artifact = artifact_or_throw(db, (*function)["artifact_id"].as_string());
    auto state = query_one(
        db, "SELECT * FROM materializations WHERE function_id=?",
        {function_handle});
    const bool cache_current =
        state && (*state)["status"].as_string() == "structure_ready" &&
        opt_string(*state, "adapter_id").value_or("") == kAdapterId &&
        opt_string(*state, "adapter_version").value_or("") == kAdapterVersionString &&
        !(*state)["analysis_schema_version"].is_null() &&
        (*state)["analysis_schema_version"].as_int() == kAnalysisSchemaVersion &&
        capability_manifest_current(db, function_handle);
    if (!refresh && cache_current) {
      Envelope env;
      env.command = "materialize";
      env.investigation = investigation_id(db);
      env.target = function_handle;
      env.result =
          llvm::json::Object{{"status", "structure_ready"}, {"cached", true}};
      env.evidence = evidence_refs(artifact["id"].as_string(),
                                   {opt_string(*state, "analysis_run_id")});
      return env.build();
    }
    if (state && (*state)["status"].as_string() == "declaration_only") {
      // Declarations carry no structure; say so instead of inventing it.
      Envelope env;
      env.command = "materialize";
      env.investigation = investigation_id(db);
      env.target = function_handle;
      env.result =
          llvm::json::Object{{"status", "declaration_only"}, {"cached", true}};
      env.evidence = evidence_refs(artifact["id"].as_string(), {});
      return env.build();
    }

    const std::string local_key = (*function)["local_key"].as_string();
    const std::string key = local_key.substr(local_key.find(':') + 1);
    const std::string stored =
        (store_.state_dir_path() / artifact["stored_path"].as_string()).string();
    const std::string owner = uuid4();
    std::string run;
    {
      SQLite::Transaction transaction(db);
      exec(db,
           "UPDATE materializations SET status='materializing',claim_owner=?,"
           "claim_started_at=?,error_json=NULL WHERE function_id=? AND "
           "(status!='materializing' OR "
           "julianday(claim_started_at)<julianday('now','-10 minutes'))",
           {owner, now_iso8601(), function_handle});
      const auto changed = query_one(db, "SELECT changes() AS n");
      if (!changed || changed->at("n").as_int() != 1)
        throw Busy("function is being materialized by another process");
      run = store_.run_start(db, "irez-llvm-index", artifact["id"].as_string(),
                             {"function", "--function-key", key});
      transaction.commit();
    }
    adapter::Result result;
    try {
      result = adapter::function_graph(stored, key);
    } catch (const std::exception &error) {
      SQLite::Transaction failed(db);
      store_.run_end(db, run, "failed",
                     llvm::json::Object{{"message", error.what()}});
      exec(db,
           "UPDATE materializations SET status='failed',error_json=?,claim_owner=NULL,"
           "claim_started_at=NULL WHERE function_id=? AND claim_owner=?",
           {json_to_string(llvm::json::Object{{"message", error.what()}}),
            function_handle, owner});
      failed.commit();
      throw;
    }
    for (const auto &record : result.records) {
      if (record.getString("record") != "error")
        continue;
      const std::string message =
          record.getString("message").value_or("adapter error").str();
      SQLite::Transaction failed(db);
      store_.run_end(db, run, "failed", llvm::json::Object{{"message", message}});
      exec(db,
           "UPDATE materializations SET status='failed',error_json=?,claim_owner=NULL,"
           "claim_started_at=NULL WHERE function_id=? AND claim_owner=?",
           {json_to_string(llvm::json::Object{{"message", message}}),
            function_handle, owner});
      failed.commit();
      throw AdapterFailure(message);
    }
    std::int64_t entity_count = 0;
    std::int64_t relation_count = 0;
    try {
      SQLite::Transaction transaction(db);
      std::map<std::string, std::string> local_to_id;
      std::vector<const llvm::json::Object *> entity_records;
      std::vector<const llvm::json::Object *> relation_records;
      std::vector<const llvm::json::Object *> source_records;
      for (const auto &record : result.records) {
        const auto kind = record.getString("record").value_or("");
        if (kind == "entity")
          entity_records.push_back(&record);
        else if (kind == "relation")
          relation_records.push_back(&record);
        else if (kind == "source")
          source_records.push_back(&record);
      }
      std::set<std::string> declared_locals;
      for (const auto *record : entity_records) {
        const std::string local = record->getString("local_key").value_or("").str();
        if (local.empty() || !declared_locals.insert(local).second)
          throw AdapterFailure("adapter emitted an empty or duplicate entity local_key: " +
                               local);
      }
      for (const auto *record : relation_records) {
        if (record->getString("kind").value_or("").empty() ||
            record->getString("src").value_or("").empty() ||
            record->getString("dst").value_or("").empty())
          throw AdapterFailure("adapter emitted a relation with missing kind or endpoint");
      }
      const std::string artifact_id = artifact["id"].as_string();
      // A refresh replaces the function snapshot.  Keeping entities that the
      // new adapter did not emit would mix analyzer generations.
      exec(db,
           "DELETE FROM relations WHERE function_id=? OR src_id IN "
           "(SELECT id FROM entities WHERE function_id=?) OR dst_id IN "
           "(SELECT id FROM entities WHERE function_id=?)",
           {function_handle, function_handle, function_handle});
      exec(db,
           "DELETE FROM source_locations WHERE entity_id IN "
           "(SELECT id FROM entities WHERE function_id=?)",
           {function_handle});
      exec(db, "DELETE FROM entities WHERE function_id=? AND id!=?",
           {function_handle, function_handle});
      for (const auto *record : entity_records) {
        const std::string local = record->getString("local_key").value_or("").str();
        const std::string id = entity_id(artifact, local);
        local_to_id[local] = id;
        llvm::json::Value attributes = llvm::json::Object{};
        if (const auto *a = record->getObject("attributes"))
          attributes = llvm::json::Value(llvm::json::Object(*a));
        exec(db,
             "INSERT INTO entities "
             "(id,artifact_id,kind,local_key,ordinal,name,opcode,llvm_type,"
             " materialization,exact_text,attributes_json) "
             "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
             "ON CONFLICT(artifact_id,local_key) DO UPDATE SET "
             " materialization='structure_ready',exact_text=excluded.exact_text,"
             " opcode=excluded.opcode,llvm_type=excluded.llvm_type,"
             " attributes_json=excluded.attributes_json",
             {id, artifact_id, record->getString("kind").value_or("").str(), local,
              json_int_param(*record, "ordinal"), json_string_param(*record, "name"),
              json_string_param(*record, "opcode"),
              json_string_param(*record, "llvm_type"), "structure_ready",
              json_string_param(*record, "exact_text"),
              json_to_string(attributes)});
        ++entity_count;
      }
      for (const auto *record : relation_records) {
        for (const char *end : {"src", "dst"}) {
          const std::string local = record->getString(end).value_or("").str();
          if (local_to_id.count(local))
            continue;
          auto existing = query_one(
              db, "SELECT id FROM entities WHERE artifact_id=? AND local_key=?",
              {artifact_id, local});
          if (existing)
            local_to_id[local] = (*existing)["id"].as_string();
        }
      }
      for (const auto *record : relation_records)
        for (const char *end : {"src", "dst"}) {
          const std::string local = record->getString(end).value_or("").str();
          if (!local_to_id.count(local))
            throw AdapterFailure("relation endpoint was not declared: " + local);
        }
      for (const auto *record : source_records) {
        const std::string local = record->getString("entity").value_or("").str();
        if (!local_to_id.count(local))
          throw AdapterFailure("source endpoint was not declared: " + local);
      }
      const std::string function_key = "function:" + key;
      const std::string function_id = local_to_id.count(function_key)
                                          ? local_to_id[function_key]
                                          : function_handle;
      for (const auto *record : entity_records) {
        const std::string kind = record->getString("kind").value_or("").str();
        if (kind == "function")
          continue;
        if (kind == "global")
          continue; // globals belong to the module, not a function (bug B6)
        const std::string id = local_to_id[record->getString("local_key")
                                               .value_or("")
                                               .str()];
        std::optional<std::string> block_id;
        if (kind == "instruction") {
          const std::string local = record->getString("local_key").value_or("").str();
          for (const auto *relation : relation_records) {
            if (relation->getString("kind") == "llvm.contains" &&
                relation->getString("dst") == local &&
                relation->getString("src").value_or("").starts_with("block:")) {
              const std::string block_local =
                  relation->getString("src").value_or("").str();
              if (local_to_id.count(block_local))
                block_id = local_to_id[block_local];
              break;
            }
          }
        }
        exec(db, "UPDATE entities SET function_id=?,block_id=? WHERE id=?",
             {function_id,
              block_id ? Param(*block_id) : Param(nullptr), id});
      }
      for (const auto *record : relation_records) {
        const std::string src_local = record->getString("src").value_or("").str();
        const std::string dst_local = record->getString("dst").value_or("").str();
        llvm::json::Value attributes = llvm::json::Object{};
        if (const auto *a = record->getObject("attributes"))
          attributes = llvm::json::Value(llvm::json::Object(*a));
        exec(db,
             "INSERT INTO relations "
             "(artifact_id,function_id,src_id,dst_id,kind,ordinal,analysis_run_id,"
             " modality,precision,attributes_json) VALUES(?,?,?,?,?,?,?,?,?,?)",
             {artifact_id, function_id, local_to_id[src_local], local_to_id[dst_local],
              record->getString("kind").value_or("").str(),
              json_int_param(*record, "ordinal"), run,
              record->getString("modality").value_or("must").str(),
              record->getString("precision").value_or("exact").str(),
              json_to_string(attributes)});
        ++relation_count;
      }
      for (const auto *record : source_records) {
        const std::string entity_local =
            record->getString("entity").value_or("").str();
        exec(db,
             "INSERT INTO source_locations "
             "(artifact_id,entity_id,file,line,column_no,discriminator,inline_depth,"
             " scope_name,analysis_run_id) VALUES(?,?,?,?,?,?,?,?,?)",
             {artifact_id, local_to_id[entity_local],
              record->getString("file").value_or("").str(),
              json_int_param(*record, "line"), json_int_param(*record, "column"),
              json_int_param(*record, "discriminator"),
              json_int_param(*record, "inline_depth"),
              record->getString("scope").value_or("").str(), run});
      }
      exec(db, "UPDATE entities SET materialization='structure_ready' WHERE id=?",
           {function_id});
      exec(db, "DELETE FROM materialization_capabilities WHERE function_id=?",
           {function_id});
      for (const auto &[capability, precision] : kMaterializationCapabilities)
        exec(db,
             "INSERT INTO materialization_capabilities "
             "(function_id,capability,status,precision,analyzer,analyzer_version,"
             "analysis_schema_version,analysis_run_id,completed_at) "
             "VALUES(?,?,?,?,?,?,?,?,?)",
             {function_id, capability, "completed", precision, kAdapterId,
              kAdapterVersionString,
              std::to_string(kAnalysisSchemaVersion), run, now_iso8601()});
      exec(db,
           "UPDATE materializations SET status='structure_ready',analysis_run_id=?,"
           "claim_owner=NULL,claim_started_at=NULL,adapter_id=?,"
           "adapter_version=?,analysis_schema_version=? "
           "WHERE function_id=? AND claim_owner=?",
           {run, kAdapterId, kAdapterVersionString,
            std::to_string(kAnalysisSchemaVersion), function_id, owner});
      store_.run_end(db, run, "success");
      transaction.commit();
    } catch (const std::exception &exc) {
      SQLite::Transaction failure(db);
      store_.run_end(db, run, "failed",
                     llvm::json::Object{{"message", exc.what()}});
      exec(db,
           "UPDATE materializations SET status='failed',error_json=?,claim_owner=NULL,"
           "claim_started_at=NULL WHERE function_id=? AND claim_owner=?",
           {json_to_string(llvm::json::Object{{"message", exc.what()}}),
            function_handle, owner});
      failure.commit();
      throw;
    }

    Envelope env;
    env.command = "materialize";
    env.investigation = investigation_id(db);
    env.target = function_handle;
    env.result = llvm::json::Object{{"status", "structure_ready"},
                                    {"cached", false},
                                    {"entities", entity_count},
                                    {"relations", relation_count}};
    env.evidence = evidence_refs(artifact["id"].as_string(), {run});
    return env.build();
  }
}

llvm::json::Object Service::show(const std::string &handle) {
  return show(handle, "summary", std::nullopt, 100);
}

llvm::json::Object Service::show(const std::string &handle,
                                const std::string &view,
                                const std::optional<std::string> &kind,
                                std::int64_t budget) {
  SQLite::Database db = store_.connect();
  ensure_materialized(db, handle);
  auto entity = query_one(db, "SELECT * FROM entities WHERE id=?", {handle});
  if (!entity)
    throw NotFound("entity not found");
  if (view != "summary" && view != "exact" && view != "children")
    throw IrezError("invalid show view: " + view, 2);
  if (budget < 0)
    throw IrezError("--budget-nodes must be non-negative", 2);
  const bool is_function = (*entity)["kind"].as_string() == "function";
  if (view == "children" && !is_function)
    throw IrezError("show --view children requires a function handle", 2);
  if (kind && view != "children")
    throw IrezError("show --kind requires --view children", 2);

  llvm::json::Object result;
  llvm::json::Array relations;
  std::vector<std::optional<std::string>> run_ids;
  std::vector<std::string> kinds;
  bool truncated = false;
  llvm::json::Array boundaries;

  if (!is_function || view == "exact") {
    result = entity_json(*entity);
    for (const auto &row : query_all(
             db,
             "SELECT kind,src_id,dst_id,ordinal,modality,precision,attributes_json,"
             "analysis_run_id FROM relations WHERE src_id=? ORDER BY kind,ordinal,dst_id",
             {handle})) {
      run_ids.push_back(opt_string(row, "analysis_run_id"));
      kinds.push_back(row.at("kind").as_string());
      Row trimmed = row;
      trimmed.erase("analysis_run_id");
      relations.push_back(relation_json(trimmed));
    }
    result["relations"] = std::move(relations);
  } else if (view == "summary") {
    result["handle"] = handle;
    result["id"] = handle;
    result["kind"] = "function";
    result["name"] = cell_to_json((*entity)["name"]);
    result["signature"] = cell_to_json((*entity)["llvm_type"]);
    result["declaration"] =
        (*entity)["materialization"].as_string() == "declaration_only";
    result["materialization"] = cell_to_json((*entity)["materialization"]);
    llvm::json::Object counts;
    counts["blocks"] = (*query_one(
        db, "SELECT count(*) AS n FROM entities WHERE function_id=? AND kind='basic_block'",
        {handle}))["n"].as_int();
    counts["instructions"] = (*query_one(
        db, "SELECT count(*) AS n FROM entities WHERE function_id=? AND kind='instruction'",
        {handle}))["n"].as_int();
    counts["returns"] = (*query_one(
        db, "SELECT count(*) AS n FROM entities WHERE function_id=? AND opcode='ret'",
        {handle}))["n"].as_int();
    counts["calls"] = (*query_one(
        db, "SELECT count(*) AS n FROM entities WHERE function_id=? AND opcode='call'",
        {handle}))["n"].as_int();
    result["counts"] = std::move(counts);
    llvm::json::Object source_summary;
    source_summary["locations"] = (*query_one(
        db, "SELECT count(*) AS n FROM source_locations s JOIN entities e "
            "ON e.id=s.entity_id WHERE e.function_id=? OR e.id=?", {handle, handle}))
        ["n"].as_int();
    llvm::json::Array files;
    for (const auto &row : query_all(
             db, "SELECT DISTINCT s.file FROM source_locations s JOIN entities e "
                 "ON e.id=s.entity_id WHERE e.function_id=? OR e.id=? "
                 "ORDER BY s.file LIMIT 5", {handle, handle}))
      files.push_back(row.at("file").as_string());
    source_summary["files"] = std::move(files);
    result["source_summary"] = std::move(source_summary);
    for (const auto &row : query_all(
             db, "SELECT DISTINCT analysis_run_id,kind FROM relations "
                 "WHERE function_id=? ORDER BY kind", {handle})) {
      run_ids.push_back(opt_string(row, "analysis_run_id"));
      kinds.push_back(row.at("kind").as_string());
    }
  } else {
    const std::string child_kind = kind.value_or("block");
    if (child_kind != "block" && child_kind != "return" && child_kind != "call")
      throw IrezError("invalid show child kind: " + child_kind, 2);
    std::string predicate = "e.kind='basic_block'";
    if (child_kind == "return")
      predicate = "e.opcode='ret'";
    else if (child_kind == "call")
      predicate = "e.opcode='call'";
    const auto children = query_all(
        db, "SELECT e.* FROM entities e WHERE e.function_id=? AND " + predicate +
                " ORDER BY e.ordinal,e.id LIMIT ?", {handle, budget + 1});
    llvm::json::Array items;
    for (std::size_t i = 0; i < children.size(); ++i) {
      if (static_cast<std::int64_t>(i) >= budget) {
        boundaries.push_back(children[i].at("id").as_string());
        continue;
      }
      items.push_back(entity_json(children[i]));
    }
    truncated = static_cast<std::int64_t>(children.size()) > budget;
    result["function"] = handle;
    result["kind"] = child_kind;
    result["items"] = std::move(items);
  }
  Envelope env;
  env.command = "show";
  env.investigation = investigation_id(db);
  env.target = handle;
  env.result = std::move(result);
  env.truncated = truncated;
  env.budget = budget;
  env.visited = truncated ? budget : 0;
  if (truncated)
    env.reason = "node_budget";
  env.boundaries = std::move(boundaries);
  if ((*entity)["kind"].as_string() == "function" &&
      (*entity)["materialization"].as_string() == "catalog_only")
    env.expandable = llvm::json::Array{handle};
  env.capabilities = relation_capabilities(kinds);
  env.evidence =
      evidence_refs((*entity)["artifact_id"].as_string(), run_ids);
  return env.build();
}

llvm::json::Object Service::source(const std::string &handle) {
  SQLite::Database db = store_.connect();
  return source_on(db, handle);
}

llvm::json::Object Service::source_on(SQLite::Database &db,
                                      const std::string &handle) {
  ensure_materialized(db, handle);
  auto entity = query_one(db, "SELECT * FROM entities WHERE id=?", {handle});
  if (!entity)
    throw NotFound("entity not found");
  llvm::json::Array rows;
  std::vector<std::optional<std::string>> run_ids;
  for (const auto &row : query_all(
           db,
           "SELECT file,line,column_no,discriminator,inline_depth,scope_name,"
           "analysis_run_id FROM source_locations WHERE entity_id=? "
           "ORDER BY inline_depth",
           {handle})) {
    run_ids.push_back(opt_string(row, "analysis_run_id"));
    Row trimmed = row;
    trimmed.erase("analysis_run_id");
    rows.push_back(row_to_json(trimmed));
  }
  const bool has_rows = !rows.empty();
  Envelope env;
  env.command = "source";
  env.investigation = investigation_id(db);
  env.target = handle;
  env.result = std::move(rows);
  if (!has_rows)
    env.unknowns = llvm::json::Array{
        llvm::json::Object{{"kind", "source_mapping"}, {"status", "unavailable"}}};
  env.capabilities = llvm::json::Array{llvm::json::Object{
      {"name", "source_mapping"}, {"status", "supported"}, {"precision", "partial"}}};
  env.evidence = evidence_refs((*entity)["artifact_id"].as_string(), run_ids);
  return env.build();
}

llvm::json::Object Service::uses(const std::string &handle, std::int64_t budget) {
  SQLite::Database db = store_.connect();
  ensure_materialized(db, handle);
  auto entity = query_one(db, "SELECT artifact_id FROM entities WHERE id=?", {handle});
  if (!entity)
    throw NotFound("entity not found");
  const auto rows = query_all(
      db,
      "SELECT r.src_id AS user,r.ordinal,e.opcode,e.exact_text,r.analysis_run_id "
      "FROM relations r JOIN entities e ON e.id=r.src_id "
      "WHERE r.dst_id=? AND r.kind='llvm.operand' "
      "ORDER BY r.src_id,r.ordinal LIMIT ?",
      {handle, budget + 1});
  const bool truncated = static_cast<std::int64_t>(rows.size()) > budget;
  llvm::json::Array visible;
  llvm::json::Array boundaries;
  std::vector<std::optional<std::string>> run_ids;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (static_cast<std::int64_t>(i) >= budget) {
      boundaries.push_back(rows[i].at("user").as_string());
      continue;
    }
    run_ids.push_back(opt_string(rows[i], "analysis_run_id"));
    Row trimmed = rows[i];
    trimmed.erase("analysis_run_id");
    visible.push_back(row_to_json(trimmed));
  }
  Envelope env;
  env.command = "uses";
  env.investigation = investigation_id(db);
  env.target = handle;
  env.result = std::move(visible);
  env.visited = truncated ? budget : static_cast<std::int64_t>(rows.size());
  env.budget = budget;
  env.truncated = truncated;
  if (truncated)
    env.reason = "node_budget";
  env.boundaries = std::move(boundaries);
  env.capabilities = relation_capabilities({"llvm.operand"});
  env.evidence = evidence_refs((*entity)["artifact_id"].as_string(), run_ids);
  return env.build();
}

llvm::json::Object Service::slice(const std::string &handle,
                                  const std::string &direction,
                                  const std::vector<std::string> &relations,
                                  std::int64_t budget_nodes, std::int64_t budget_depth) {
  SQLite::Database db = store_.connect();
  return slice_on(db, handle, direction, relations, budget_nodes, budget_depth);
}

llvm::json::Object Service::slice_on(SQLite::Database &db,
                                     const std::string &handle,
                                     const std::string &direction,
                                     const std::vector<std::string> &relations,
                                     std::int64_t budget_nodes,
                                     std::int64_t budget_depth) {
  ensure_materialized(db, handle);
  auto target = query_one(db, "SELECT * FROM entities WHERE id=?", {handle});
  if (!target)
    throw NotFound("entity not found");

  std::optional<std::string> function_id = opt_string(*target, "function_id");
  if (!function_id && (*target)["kind"].as_string() == "function")
    function_id = handle;

  std::set<std::string> kinds;
  const std::set<std::string> supported_relations = {
      "llvm.operand", "llvm.control-dependence", "llvm.calls",
      "llvm.cfg-successor", "llvm.contains", "llvm.references-global"};
  for (const std::string &relation : relations) {
    auto it = kRelationAliases.find(relation);
    const std::string kind = it == kRelationAliases.end() ? relation : it->second;
    if (!supported_relations.count(kind))
      throw IrezError("unknown slice relation: " + relation, 2);
    kinds.insert(kind);
  }
  llvm::json::Array unknowns;
  if (!function_id) {
    // Module-scoped entities (globals, ...) have no function body whose
    // relations could be traversed: the loop below returns the target node
    // alone. Report that explicitly instead of letting nodes:1/relations:0
    // look like "no outgoing relations exist" (V00_00 bug class B2; release
    // checklist A3). Artifact-wide global traversal would need its own
    // budget and function-boundary design; until then, stay honest.
    unknowns.push_back(llvm::json::Object{
        {"kind", "scope"},
        {"status", "unsupported"},
        {"reason", "slice traversal is function-scoped; this entity belongs to "
                   "no function, so only the target node is returned and no "
                   "relations are traversed"},
        {"hint", "use uses/show for the direct neighbors of module-scoped "
                 "entities"}});
  }
  if (kinds.count("llvm.control-dependence")) {
    const bool cd_completed = function_id && query_one(
        db, "SELECT 1 AS ok FROM materialization_capabilities WHERE function_id=? "
            "AND capability='control_dependence' AND status='completed'",
        {*function_id}).has_value();
    if (!cd_completed) {
      kinds.erase("llvm.control-dependence");
      unknowns.push_back(llvm::json::Object{
          {"relation", "llvm.control-dependence"},
          {"status", "unsupported"},
          {"reason", "function has no completed control-dependence capability; "
                     "reindex with a CD-capable adapter"}});
    }
  }

  // Backend-bounded traversal: the BFS reads only the adjacency of the
  // current frontier node through the relations_(src|dst)_kind indexes, and
  // entities are fetched afterwards for the visited ids alone. The previous
  // implementation loaded every relation of the function and every entity of
  // the artifact into memory first, so "bounded output" did not mean bounded
  // backend work on large modules (and trace_return repeated that load once
  // per return site).
  const std::string frontier_column =
      direction == "backward" ? "src_id" : "dst_id";
  std::string kind_list;
  for (const std::string &kind : kinds) {
    if (!kind_list.empty())
      kind_list += ",";
    // kinds are validated against supported_relations above: no quoting risk.
    kind_list += "'" + kind + "'";
  }
  // Ordering matches the old in-memory comparator (kind, ordinal with NULLs
  // first, src_id, dst_id), so traversal order and responses are unchanged.
  const std::string adjacency_sql =
      "SELECT * FROM relations WHERE function_id=? AND " + frontier_column +
      "=? AND kind IN (" + kind_list + ") ORDER BY kind,ordinal,src_id,dst_id";

  std::deque<std::pair<std::string, std::int64_t>> queue;
  queue.emplace_back(handle, 0);
  std::vector<std::string> seen;
  std::set<std::string> known{handle};
  llvm::json::Array selected_edges;
  std::vector<std::optional<std::string>> selected_run_ids;
  std::vector<std::string> boundaries;
  std::optional<std::string> reason;
  while (!queue.empty()) {
    const auto [node, depth] = queue.front();
    queue.pop_front();
    seen.push_back(node);
    if (!function_id || kinds.empty())
      continue;
    for (const Row &edge : query_all(db, adjacency_sql, {*function_id, node})) {
      const std::string &nxt = direction == "backward"
                                   ? edge.at("dst_id").as_string()
                                   : edge.at("src_id").as_string();
      if (depth >= budget_depth) {
        // Only unexplored nodes are boundaries; an already-known node reached
        // at maximum depth still contributes its edge (V00_00 bug B4).
        if (!known.count(nxt)) {
          boundaries.push_back(nxt);
          if (!reason)
            reason = "depth_budget";
          continue;
        }
      } else if (!known.count(nxt) &&
                 static_cast<std::int64_t>(known.size()) >= budget_nodes) {
        boundaries.push_back(nxt);
        if (!reason)
          reason = "node_budget";
        continue;
      }
      llvm::json::Object selected;
      for (const char *key :
           {"src_id", "dst_id", "kind", "ordinal", "modality", "precision"})
        selected[key] = cell_to_json(edge.at(key));
      selected["attributes"] = parse_json(
          edge.at("attributes_json").as_string(), "relation attributes_json");
      selected_edges.push_back(std::move(selected));
      selected_run_ids.push_back(opt_string(edge, "analysis_run_id"));
      if (!known.count(nxt)) {
        known.insert(nxt);
        queue.emplace_back(nxt, depth + 1);
      }
    }
  }

  // Fetch entities for the visited ids only, in bounded IN(...) batches.
  std::map<std::string, llvm::json::Object> entities;
  constexpr std::size_t kEntityBatch = 500;
  for (std::size_t begin = 0; begin < seen.size(); begin += kEntityBatch) {
    const std::size_t end = std::min(begin + kEntityBatch, seen.size());
    std::string placeholders;
    std::vector<Param> ids;
    ids.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (!placeholders.empty())
        placeholders += ",";
      placeholders += "?";
      ids.push_back(seen[i]);
    }
    for (const auto &row : query_all(
             db, "SELECT * FROM entities WHERE id IN (" + placeholders + ")",
             ids))
      entities[row.at("id").as_string()] = entity_json(row);
  }

  llvm::json::Array nodes;
  for (const std::string &node : seen)
    if (entities.count(node))
      nodes.push_back(llvm::json::Value(llvm::json::Object(entities[node])));

  Envelope env;
  env.command = "slice";
  env.investigation = investigation_id(db);
  env.target = handle;
  env.result = llvm::json::Object{{"nodes", std::move(nodes)},
                                  {"relations", std::move(selected_edges)},
                                  {"direction", direction},
                                  {"budget_depth", budget_depth}};
  env.unknowns = std::move(unknowns);
  std::vector<std::string> unique_boundaries = sorted_unique(boundaries);
  env.boundaries = llvm::json::Array{};
  for (const std::string &boundary : unique_boundaries)
    env.boundaries.getAsArray()->push_back(boundary);
  env.visited = static_cast<std::int64_t>(seen.size());
  env.budget = budget_nodes;
  env.truncated = !boundaries.empty();
  env.reason = reason;
  env.expandable = env.boundaries;
  env.capabilities =
      relation_capabilities(std::vector<std::string>(kinds.begin(), kinds.end()));
  env.evidence =
      evidence_refs((*target)["artifact_id"].as_string(), selected_run_ids);
  return env.build();
}

llvm::json::Object Service::graph(const std::string &handle,
                                  const std::string &direction, std::int64_t budget,
                                  const std::string &format) {
  llvm::json::Object response =
      slice(handle, direction, {"operand"}, budget, 12);
  response["command"] = "graph";
  if (format == "exact-ir") {
    llvm::json::Array rendered;
    if (auto *result = response.getObject("result"))
      if (auto *nodes = result->getArray("nodes"))
        for (const auto &node : *nodes) {
          const auto *object = node.getAsObject();
          if (!object)
            continue;
          rendered.push_back(llvm::json::Object{
              {"handle", object->get("id") ? *object->get("id")
                                           : llvm::json::Value(nullptr)},
              {"exact_ir", object->get("exact_text")
                               ? *object->get("exact_text")
                               : llvm::json::Value(nullptr)}});
        }
    response["result"] = std::move(rendered);
  }
  return response;
}

llvm::json::Object Service::guards(const std::string &handle, std::int64_t budget) {
  SQLite::Database db = store_.connect();
  ensure_materialized(db, handle);
  auto target = query_one(db, "SELECT * FROM entities WHERE id=?", {handle});
  if (!target)
    throw NotFound("entity not found");
  const std::string block = (*target)["kind"].as_string() == "instruction"
                                ? opt_string(*target, "block_id").value_or(handle)
                                : handle;
  llvm::json::Array preds;
  std::vector<std::optional<std::string>> run_ids;

  // Exact path: this function has an explicit completed capability, so the
  // guards of a block are exactly the branches it is control-dependent on.
  // Fallback (legacy state): conservative immediate-CFG-predecessor evidence.
  std::optional<std::string> function_id = opt_string(*target, "function_id");
  if (!function_id && (*target)["kind"].as_string() == "function")
    function_id = handle;
  const bool artifact_has_cd = function_id && query_one(
      db, "SELECT 1 AS ok FROM materialization_capabilities WHERE function_id=? "
          "AND capability='control_dependence' AND status='completed'",
      {*function_id}).has_value();

  auto branch_predicate = [&](const std::string &terminator) {
    // A branch predicate only exists for a conditional br (two successors).
    // An unconditional br's first operand is the destination block, not a
    // predicate (V00_00 bug B3).
    llvm::json::Value predicate(nullptr);
    auto opcode = query_one(db, "SELECT opcode FROM entities WHERE id=?",
                            {terminator});
    if (opcode && ((*opcode)["opcode"].as_string() == "br" ||
                   (*opcode)["opcode"].as_string() == "switch")) {
      const std::int64_t successors =
          (*query_one(db,
                      "SELECT count(*) AS n FROM relations WHERE src_id=? AND "
                      "kind='llvm.cfg-successor'",
                      {terminator}))["n"]
              .as_int();
      if (((*opcode)["opcode"].as_string() == "br" && successors == 2) ||
          (*opcode)["opcode"].as_string() == "switch") {
        auto operand = query_one(
            db,
            "SELECT dst_id FROM relations WHERE src_id=? AND kind='llvm.operand' "
            "AND ordinal=0",
            {terminator});
        if (operand)
          predicate = (*operand)["dst_id"].as_string();
      }
    }
    return predicate;
  };

  // Fetch one row beyond the budget so truncation is detected exactly.
  // Previously the query used LIMIT budget and always reported
  // truncated=false, silently dropping guards beyond the budget and
  // violating the honesty invariant (budget=0 returned an empty result
  // with no truncation signal at all).
  std::vector<Row> rows;
  if (artifact_has_cd) {
    rows = query_all(
        db,
        "SELECT r.dst_id AS terminator,r.ordinal AS required_successor,"
        "e.exact_text,e.opcode,r.attributes_json,r.analysis_run_id "
        "FROM relations r JOIN entities e ON e.id=r.dst_id "
        "WHERE r.kind='llvm.control-dependence' AND r.src_id=? "
        "ORDER BY r.dst_id,r.ordinal LIMIT ?",
        {block, budget + 1});
  } else {
    rows = query_all(
        db,
        "SELECT r.src_id AS terminator,r.ordinal AS required_successor,"
        "e.exact_text,e.opcode,r.attributes_json,r.analysis_run_id "
        "FROM relations r JOIN entities e ON e.id=r.src_id "
        "WHERE r.kind='llvm.cfg-successor' AND r.dst_id=? "
        "ORDER BY r.src_id,r.ordinal LIMIT ?",
        {block, budget + 1});
  }
  const bool truncated = static_cast<std::int64_t>(rows.size()) > budget;
  llvm::json::Array boundaries;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    Row row = std::move(rows[i]);
    if (static_cast<std::int64_t>(i) >= budget) {
      // Beyond-budget rows are evidence that remains expandable; expose the
      // terminator handle as a boundary instead of dropping it silently.
      boundaries.push_back(row.at("terminator").as_string());
      continue;
    }
    run_ids.push_back(opt_string(row, "analysis_run_id"));
    row.erase("analysis_run_id");
    llvm::json::Object item = row_to_json(row);
    item["attributes"] = parse_json(
        row.at("attributes_json").as_string(),
        artifact_has_cd ? "control dependence attributes" : "CFG attributes");
    item.erase("attributes_json");
    item["predicate"] = branch_predicate(row.at("terminator").as_string());
    preds.push_back(std::move(item));
  }
  const std::int64_t pred_count = static_cast<std::int64_t>(preds.size());
  Envelope env;
  env.command = "guards";
  env.investigation = investigation_id(db);
  env.target = handle;
  env.result = std::move(preds);
  if (artifact_has_cd) {
    env.capabilities = llvm::json::Array{llvm::json::Object{
        {"name", "control_guards"}, {"status", "supported"}, {"precision", "exact"}}};
  } else {
    env.capabilities = llvm::json::Array{llvm::json::Object{
        {"name", "control_guards"}, {"status", "partial"}, {"precision", "conservative"}}};
    env.unknowns = llvm::json::Array{llvm::json::Object{
        {"kind", "control_dependence"},
        {"reason", "function has no completed control-dependence capability; "
                   "reindex with a CD-capable adapter for exact guards"}}};
  }
  env.visited = truncated ? budget : pred_count;
  env.budget = budget;
  env.truncated = truncated;
  if (truncated)
    env.reason = "node_budget";
  env.boundaries = std::move(boundaries);
  env.evidence = evidence_refs((*target)["artifact_id"].as_string(), run_ids);
  return env.build();
}

llvm::json::Object Service::expand(const std::string &handle) {
  SQLite::Database db = store_.connect();
  auto entity = query_one(db, "SELECT * FROM entities WHERE id=?", {handle});
  if (!entity)
    throw NotFound("entity not found");
  if ((*entity)["kind"].as_string() == "function") {
    llvm::json::Object response = materialize(handle);
    response["command"] = "expand";
    return response;
  }
  // Column aliases matter: relation and entity both have kind/attributes_json
  // columns; V00_00 bug B1 came from ambiguous duplicate names.
  auto call = query_one(
      db,
      "SELECT r.dst_id AS dst_id, r.attributes_json AS rel_attributes, "
      "r.analysis_run_id AS rel_run, e.kind AS entity_kind "
      "FROM relations r JOIN entities e ON e.id=r.dst_id "
      "WHERE r.src_id=? AND r.kind='llvm.calls'",
      {handle});
  // Match Python: the first call relation wins; zero means unknown.
  if (!call) {
    Envelope env;
    env.command = "expand";
    env.investigation = investigation_id(db);
    env.target = handle;
    env.result =
        llvm::json::Object{{"status", "unknown"}, {"expandable", false}};
    env.unknowns = llvm::json::Array{
        llvm::json::Object{{"kind", "indirect_or_non_call"}}};
    env.evidence = llvm::json::Array{(*entity)["artifact_id"].as_string()};
    return env.build();
  }
  bool external = false;
  if (auto attributes = opt_string(*call, "rel_attributes")) {
    const auto parsed = parse_json(*attributes, "call relation attributes");
    if (const auto *object = parsed.getAsObject())
      external = object->getBoolean("external").value_or(false);
  }
  if (external || (*call)["entity_kind"].as_string() != "function") {
    Envelope env;
    env.command = "expand";
    env.investigation = investigation_id(db);
    env.target = handle;
    env.result = llvm::json::Object{{"status", "external"},
                                    {"effects", "unknown"},
                                    {"expandable", false}};
    env.capabilities = relation_capabilities({"llvm.calls"});
    env.evidence = evidence_refs((*entity)["artifact_id"].as_string(),
                                 {opt_string(*call, "rel_run")});
    return env.build();
  }
  llvm::json::Object response = materialize((*call)["dst_id"].as_string());
  response["command"] = "expand";
  return response;
}

llvm::json::Object Service::capabilities(const std::optional<std::string> &artifact) {
  SQLite::Database db = store_.connect();
  const Row art = artifact_or_throw(db, artifact);
  const auto metadata =
      parse_json(art.at("metadata_json").as_string(), "artifact metadata_json");
  llvm::json::Value result = llvm::json::Object{};
  if (const auto *object = metadata.getAsObject())
    if (const auto *caps = object->getObject("capabilities"))
      if (const auto *inner = caps->getObject("capabilities"))
        result = own_json(llvm::json::Value(llvm::json::Object(*inner)));
  Envelope env;
  env.command = "capabilities";
  env.investigation = investigation_id(db);
  env.target = art.at("id").as_string();
  env.result = std::move(result);
  return env.build();
}

llvm::json::Object Service::context(const std::string &handle, std::int64_t budget) {
  llvm::json::Object shown = show(handle);
  llvm::json::Object src = source(handle);
  llvm::json::Object value_graph = graph(handle, "backward", budget, "json");

  llvm::json::Object result;
  auto take_result = [](llvm::json::Object &object) {
    if (const auto *value = object.get("result"))
      return *value;
    return llvm::json::Value(nullptr);
  };
  result["entity"] = take_result(shown);
  result["source"] = take_result(src);
  result["value_graph"] = take_result(value_graph);
  result["observation"] =
      llvm::json::Object{{"status", "unavailable"},
                         {"reason", "no external runtime observation was provided"}};

  Envelope env;
  env.command = "context";
  if (auto inv = shown.getString("investigation"))
    env.investigation = inv->str();
  env.target = handle;
  env.result = std::move(result);

  // Python order: graph capabilities, then source capabilities.
  llvm::json::Array capabilities;
  for (const auto *object : {&value_graph, &src})
    if (const auto *used = object->getArray("capabilities_used"))
      for (const auto &capability : *used)
        capabilities.push_back(capability);
  env.capabilities = std::move(capabilities);

  if (const auto *boundaries = value_graph.getArray("boundaries"))
    env.boundaries = llvm::json::Value(llvm::json::Array(*boundaries));

  llvm::json::Array unknowns;
  for (const auto *object : {&src, &value_graph})
    if (const auto *list = object->getArray("unknowns"))
      for (const auto &unknown : *list)
        unknowns.push_back(unknown);
  unknowns.push_back(llvm::json::Object{{"kind", "runtime_observation"},
                                        {"status", "unavailable"}});
  env.unknowns = std::move(unknowns);

  if (const auto *truncation = value_graph.getObject("truncation")) {
    env.visited = truncation->getInteger("visited_nodes").value_or(0);
    env.truncated = truncation->getBoolean("truncated").value_or(false);
    if (auto r = truncation->getString("reason"))
      env.reason = r->str();
  }
  env.budget = budget;

  std::vector<std::string> evidence;
  for (const auto *object : {&shown, &src, &value_graph})
    if (const auto *refs = object->getArray("evidence_refs"))
      for (const auto &ref : *refs)
        if (auto text = ref.getAsString())
          evidence.push_back(text->str());
  llvm::json::Array unique_evidence;
  for (const std::string &ref : evidence) {
    bool seen = false;
    for (const auto &existing : unique_evidence)
      if (existing.getAsString() && *existing.getAsString() == ref)
        seen = true;
    if (!seen)
      unique_evidence.push_back(ref);
  }
  env.evidence = std::move(unique_evidence);

  if (const auto *expandable = value_graph.getArray("expandable"))
    env.expandable = llvm::json::Value(llvm::json::Array(*expandable));
  return env.build();
}

namespace {

// Projection contract: `detail` picks the default section set and an explicit
// `include` list EXTENDS it (backlog F5 — the documented "add flags on top"
// semantics; the previous override behavior silently dropped the graph
// projection when a caller only wanted flags). Per-site truncation is always
// present regardless of projection — it is a correctness contract, not
// payload.
std::set<std::string> resolve_trace_sections(
    const std::string &detail, const std::vector<std::string> &include) {
  static const std::set<std::string> kKnownSections = {
      "chain", "calls", "source", "nodes", "relations", "boundaries", "flags"};
  std::set<std::string> sections =
      detail == "summary"
          ? std::set<std::string>{"chain", "calls", "source", "boundaries"}
          : std::set<std::string>{"calls", "source", "nodes", "relations",
                                  "boundaries"};
  for (const std::string &section : include) {
    if (!kKnownSections.count(section))
      throw IrezError("unknown trace include section: " + section, 2);
    sections.insert(section);
  }
  return sections;
}

} // namespace

llvm::json::Object Service::trace_return(const std::string &function_handle,
                                         const std::string &return_selector,
                                         std::int64_t budget_nodes,
                                         std::int64_t budget_depth,
                                         const std::string &detail,
                                         const std::vector<std::string> &include) {
  SQLite::Database db = store_.connect();
  ensure_materialized(db, function_handle);
  auto function = query_one(
      db, "SELECT * FROM entities WHERE id=? AND kind='function'", {function_handle});
  if (!function)
    throw NotFound("function not found");
  if (budget_nodes < 0 || budget_depth < 0)
    throw IrezError("trace-return budgets must be non-negative", 2);
  if (detail != "graph" && detail != "summary")
    throw IrezError("trace-return detail must be graph or summary", 2);
  const std::set<std::string> sections = resolve_trace_sections(detail, include);

  std::vector<Row> returns;
  if (return_selector == "all") {
    returns = query_all(db,
                        "SELECT * FROM entities WHERE function_id=? AND opcode='ret' "
                        "ORDER BY ordinal,id",
                        {function_handle});
  } else {
    auto selected = query_one(
        db, "SELECT * FROM entities WHERE id=? AND function_id=? AND opcode='ret'",
        {return_selector, function_handle});
    if (!selected)
      throw NotFound("selected return is not a ret instruction in the function");
    returns.push_back(std::move(*selected));
  }

  // F2 companion: kernel-shaped functions (XLA/Numba/Triton) return void or a
  // constant null pointer and deliver results through stores. When no return
  // carries a real value and the function stores, point at trace-stores
  // instead of letting the empty/null chain look like missing evidence.
  bool value_return_seen = false;
  for (const Row &ret : returns) {
    auto operand = query_one(
        db, "SELECT e.kind,e.exact_text FROM relations r JOIN entities e "
            "ON e.id=r.dst_id WHERE r.src_id=? AND r.kind='llvm.operand' "
            "AND r.ordinal=0",
        {ret.at("id").as_string()});
    if (!operand)
      continue; // ret void
    const std::string text = opt_string(*operand, "exact_text").value_or("");
    const bool constant_null =
        (*operand)["kind"].as_string() == "constant" && text.size() >= 4 &&
        text.compare(text.size() - 4, 4, "null") == 0;
    if (!constant_null) {
      value_return_seen = true;
      break;
    }
  }
  llvm::json::Array extra_unknowns;
  if (!value_return_seen &&
      (*query_one(db, "SELECT count(*) AS n FROM entities WHERE function_id=? "
                     "AND opcode='store'", {function_handle}))["n"].as_int() > 0)
    extra_unknowns.push_back(llvm::json::Object{
        {"kind", "result_channel"},
        {"status", "write_side_effects"},
        {"reason", "every return is void or a constant null pointer; results "
                   "leave this function through store instructions"},
        {"hint", "use trace-stores to trace the stored values"}});

  llvm::json::Object response =
      run_trace(db, *function, std::move(returns), "return", "trace-return",
                sections, detail, budget_nodes, budget_depth);
  if (auto *unknowns = response.getArray("unknowns"))
    for (auto &item : extra_unknowns)
      unknowns->push_back(std::move(item));
  return response;
}

llvm::json::Object Service::trace_stores(const std::string &function_handle,
                                         std::int64_t budget_nodes,
                                         std::int64_t budget_depth,
                                         const std::string &detail,
                                         const std::vector<std::string> &include) {
  SQLite::Database db = store_.connect();
  ensure_materialized(db, function_handle);
  auto function = query_one(
      db, "SELECT * FROM entities WHERE id=? AND kind='function'", {function_handle});
  if (!function)
    throw NotFound("function not found");
  if (budget_nodes < 0 || budget_depth < 0)
    throw IrezError("trace-stores budgets must be non-negative", 2);
  if (detail != "graph" && detail != "summary")
    throw IrezError("trace-stores detail must be graph or summary", 2);
  const std::set<std::string> sections = resolve_trace_sections(detail, include);
  std::vector<Row> stores = query_all(
      db, "SELECT * FROM entities WHERE function_id=? AND opcode='store' "
          "ORDER BY ordinal,id",
      {function_handle});
  return run_trace(db, *function, std::move(stores), "store", "trace-stores",
                   sections, detail, budget_nodes, budget_depth);
}

llvm::json::Object Service::run_trace(SQLite::Database &db, const Row &function,
                                      std::vector<Row> sinks,
                                      const std::string &sink_kind,
                                      const std::string &command,
                                      const std::set<std::string> &sections,
                                      const std::string &detail,
                                      std::int64_t budget_nodes,
                                      std::int64_t budget_depth) {
  const bool want_chain = sections.count("chain");
  const bool want_calls = sections.count("calls");
  const bool want_source = sections.count("source");
  const bool want_nodes = sections.count("nodes");
  const bool want_relations = sections.count("relations");
  const bool want_boundaries = sections.count("boundaries");
  const bool want_flags = sections.count("flags");

  const std::string function_handle = function.at("id").as_string();
  llvm::json::Array sites;
  llvm::json::Array boundaries;
  llvm::json::Array unknowns;
  llvm::json::Array capabilities;
  llvm::json::Array evidence;
  std::set<std::string> boundary_seen;
  std::set<std::string> capability_seen;
  std::set<std::string> evidence_seen;
  bool any_truncated = false;
  std::int64_t truncated_sites = 0;
  std::set<std::string> truncation_reasons;
  std::int64_t visited_total = 0;
  const std::string artifact_id = function.at("artifact_id").as_string();
  evidence_seen.insert(artifact_id);
  evidence.push_back(artifact_id);
  if (sinks.empty())
    unknowns.push_back(llvm::json::Object{
        {"kind", sink_kind + "_sites"},
        {"status", "none"},
        {"reason", sink_kind == "return"
                       ? "function contains no ret instruction"
                       : "function contains no store instruction"}});

  auto compact_entity = [](const llvm::json::Object &object) {
    llvm::json::Object compact;
    for (const char *key : {"handle", "id", "kind", "name", "opcode", "llvm_type",
                            "function_id", "block_id", "ordinal"})
      if (const auto *value = object.get(key))
        compact[key] = own_json(*value);
    return compact;
  };

  for (const Row &sink : sinks) {
    const std::string sink_handle = sink.at("id").as_string();
    // All per-site queries share this function's connection (slice_on /
    // source_on) instead of opening a fresh connection per slice and source
    // call inside the loop.
    llvm::json::Object traced =
        slice_on(db, sink_handle, "backward", {"operand"}, budget_nodes, budget_depth);
    llvm::json::Object located = source_on(db, sink_handle);
    llvm::json::Object site;
    const llvm::json::Object sink_entity = entity_json(sink);
    site[sink_kind] = compact_entity(sink_entity);
    auto sink_operand = [&](std::int64_t ordinal) {
      return query_one(
          db, "SELECT e.* FROM relations r JOIN entities e ON e.id=r.dst_id "
              "WHERE r.src_id=? AND r.kind='llvm.operand' AND r.ordinal=?",
          {sink_handle, ordinal});
    };
    auto value_shape_of = [](const llvm::json::Object &entity) {
      // Scalar/vector shape is a structural fact derivable from the value
      // type; callers asked for it without fetching the full graph.
      const std::string llvm_type =
          entity.getString("llvm_type").value_or("").str();
      return llvm::json::Value(llvm_type.starts_with("<") ? "vector" : "scalar");
    };
    if (sink_kind == "return") {
      if (auto operand = sink_operand(0)) {
        site["operand"] = compact_entity(entity_json(*operand));
        site["value_shape"] = value_shape_of(*site["operand"].getAsObject());
      } else {
        site["operand"] = nullptr; // ret void
        site["value_shape"] = llvm::json::Value("void");
      }
    } else {
      // store: operand 0 is the stored value, operand 1 the destination
      // pointer; naming the pointer is what makes a store trace actionable.
      if (auto value = sink_operand(0)) {
        site["value"] = compact_entity(entity_json(*value));
        site["value_shape"] = value_shape_of(*site["value"].getAsObject());
      } else {
        site["value"] = nullptr; // defensive: a store always has a value operand
        site["value_shape"] = llvm::json::Value("void");
      }
      if (auto pointer = sink_operand(1))
        site["pointer"] = compact_entity(entity_json(*pointer));
      else
        site["pointer"] = nullptr;
    }

    // Compact nodes (traversal order) feed both the summary `chain` spine and
    // the graph-mode `value_graph.nodes`; the counts are always available so
    // a summary consumer knows what the full graph would cost.
    llvm::json::Array compact_nodes;
    std::int64_t node_count = 0;
    std::int64_t relation_count = 0;
    if (const auto *graph = traced.getObject("result")) {
      if (const auto *nodes = graph->getArray("nodes")) {
        node_count = static_cast<std::int64_t>(nodes->size());
        for (const auto &node : *nodes)
          if (const auto *object = node.getAsObject())
            compact_nodes.push_back(compact_entity(*object));
      }
      if (const auto *edges = graph->getArray("relations"))
        relation_count = static_cast<std::int64_t>(edges->size());
    }
    site["node_count"] = node_count;
    site["relation_count"] = relation_count;
    if (want_chain)
      site["chain"] = llvm::json::Value(llvm::json::Array(compact_nodes));
    if (want_nodes || want_relations) {
      llvm::json::Object compact_graph;
      if (const auto *graph = traced.getObject("result"))
        for (const auto &entry : *graph)
          if (entry.first != "nodes" && entry.first != "relations")
            compact_graph[entry.first] = own_json(entry.second);
      if (want_nodes)
        compact_graph["nodes"] = std::move(compact_nodes);
      if (want_relations)
        if (const auto *graph = traced.getObject("result"))
          if (const auto *edges = graph->getArray("relations"))
            compact_graph["relations"] =
                own_json(llvm::json::Value(llvm::json::Array(*edges)));
      site["value_graph"] = std::move(compact_graph);
    }
    // Opt-in flags section: sparse rows for graph nodes carrying at least one
    // non-default (true) boolean instruction attribute (nsw/exact/fast-math
    // family). Nodes with all-default flags are omitted; a requested section
    // with no flagged nodes yields an empty array so "checked, none set" is
    // distinguishable from "section not requested".
    if (want_flags) {
      llvm::json::Array flag_rows;
      if (const auto *graph = traced.getObject("result"))
        if (const auto *nodes = graph->getArray("nodes"))
          for (const auto &node : *nodes)
            if (const auto *object = node.getAsObject()) {
              // Only instruction nodes: function/argument/constant nodes in
              // the graph carry their own boolean attributes (e.g.
              // "declaration") that are not instruction flags.
              if (object->getString("opcode").value_or("").empty())
                continue;
              const auto *attributes = object->getObject("attributes");
              if (!attributes)
                continue;
              llvm::json::Object active;
              for (const auto &kv : *attributes)
                if (kv.second.getAsBoolean().value_or(false))
                  active[kv.first] = true;
              if (active.empty())
                continue;
              llvm::json::Object row;
              row["handle"] = llvm::json::Value(
                  object->getString("handle").value_or("").str());
              row["opcode"] = llvm::json::Value(
                  object->getString("opcode").value_or("").str());
              row["flags"] = std::move(active);
              flag_rows.push_back(std::move(row));
            }
      site["flags"] = std::move(flag_rows);
    }
    if (want_source)
      if (const auto *value = located.get("result"))
        site["source"] = own_json(*value);

    llvm::json::Array calls;
    llvm::json::Array call_boundaries;
    if (want_calls)
      if (const auto *graph = traced.getObject("result"))
        if (const auto *nodes = graph->getArray("nodes"))
          for (const auto &node : *nodes)
            if (const auto *object = node.getAsObject();
                object && object->getString("opcode") == "call") {
              calls.push_back(compact_entity(*object));
              const std::string call_handle = object->getString("id").value_or("").str();
              for (const auto &relation : query_all(
                       db, "SELECT dst_id,modality,precision,"
                           "analysis_run_id FROM relations WHERE src_id=? AND "
                           "kind='llvm.calls' ORDER BY ordinal,dst_id",
                       {call_handle})) {
                llvm::json::Object boundary;
                boundary["call"] = call_handle;
                boundary["target"] = relation.at("dst_id").as_string();
                boundary["modality"] = cell_to_json(relation.at("modality"));
                boundary["precision"] = cell_to_json(relation.at("precision"));
                // Name the target in the composed response: a bare handle
                // forced callers to `show` every boundary target just to
                // identify it (the rerun kept unnamed function:f5 targets).
                auto target_entity = query_one(
                    db, "SELECT name,llvm_type,materialization FROM entities WHERE id=?",
                    {relation.at("dst_id").as_string()});
                if (target_entity) {
                  boundary["target_name"] =
                      cell_to_json((*target_entity)["name"]);
                  boundary["target_llvm_type"] =
                      cell_to_json((*target_entity)["llvm_type"]);
                  // The reason distinguishes "declaration only" (genuinely
                  // external to the module) from "definition indexed but not
                  // yet materialized" (internal, expandable) — both carry a
                  // module-local function id, so status alone was ambiguous.
                  const std::string materialization =
                      (*target_entity)["materialization"].as_string();
                  boundary["reason"] = llvm::json::Value(materialization);
                  const bool external = materialization == "declaration_only";
                  boundary["status"] = external ? "external" : "internal";
                  boundary["expandable"] = !external;
                } else {
                  // An unindexed call target is unknown; reporting it as
                  // internal+expandable would send callers to a doomed
                  // expand.
                  boundary["status"] = "unknown";
                  boundary["expandable"] = false;
                  boundary["reason"] = llvm::json::Value("target_not_indexed");
                }
                call_boundaries.push_back(std::move(boundary));
                if (const auto run_id = opt_string(relation, "analysis_run_id");
                    run_id && evidence_seen.insert(*run_id).second)
                  evidence.push_back(*run_id);
              }
            }
    if (!calls.empty()) {
      llvm::json::Object direct_capability{
          {"name", "direct_calls"}, {"status", "supported"},
          {"precision", "exact"}};
      const std::string key = json_to_string(
          llvm::json::Value(llvm::json::Object(direct_capability)));
      if (capability_seen.insert(key).second)
        capabilities.push_back(std::move(direct_capability));
    }
    // Per-site truncation is part of the correctness contract: it is always
    // present, in every projection. Per-site boundaries are a payload
    // section and follow the include selection.
    if (want_boundaries) {
      llvm::json::Array site_boundaries;
      if (const auto *items = traced.getArray("boundaries"))
        for (const auto &item : *items)
          site_boundaries.push_back(own_json(item));
      site["boundaries"] = std::move(site_boundaries);
    }
    if (const auto *truncation = traced.getObject("truncation")) {
      const bool site_truncated =
          truncation->getBoolean("truncated").value_or(false);
      llvm::json::Object site_truncation;
      site_truncation["truncated"] = site_truncated;
      if (auto reason = truncation->getString("reason"))
        site_truncation["reason"] = llvm::json::Value(reason->str());
      else
        site_truncation["reason"] = llvm::json::Value(nullptr);
      site_truncation["visited_nodes"] =
          truncation->getInteger("visited_nodes").value_or(0);
      site_truncation["budget_nodes"] = budget_nodes;
      site_truncation["budget_depth"] = budget_depth;
      site["truncation"] = std::move(site_truncation);
      if (site_truncated) {
        any_truncated = true;
        ++truncated_sites;
        if (auto reason = truncation->getString("reason"))
          truncation_reasons.insert(reason->str());
      }
      visited_total += truncation->getInteger("visited_nodes").value_or(0);
    }
    if (want_calls) {
      site["direct_calls"] = std::move(calls);
      site["call_boundaries"] = std::move(call_boundaries);
    }
    sites.push_back(std::move(site));

    if (const auto *items = traced.getArray("boundaries"))
      for (const auto &item : *items)
        if (const auto text = item.getAsString();
            text && boundary_seen.insert(text->str()).second)
          boundaries.push_back(text->str());
    for (const auto *response : {&traced, &located}) {
      if (const auto *items = response->getArray("unknowns"))
        for (const auto &item : *items)
          unknowns.push_back(own_json(item));
      if (const auto *items = response->getArray("capabilities_used"))
        for (const auto &item : *items) {
          const std::string key = json_to_string(item);
          if (capability_seen.insert(key).second)
            capabilities.push_back(own_json(item));
        }
      if (const auto *items = response->getArray("evidence_refs"))
        for (const auto &item : *items)
          if (const auto text = item.getAsString();
              text && evidence_seen.insert(text->str()).second)
            evidence.push_back(text->str());
    }
  }

  Envelope env;
  env.command = command;
  env.investigation = investigation_id(db);
  env.target = function_handle;
  env.result = llvm::json::Object{
      {"function", function_handle},
      {sink_kind + "_count", static_cast<std::int64_t>(sinks.size())},
      {"sites", std::move(sites)}};
  env.capabilities = std::move(capabilities);
  env.boundaries = std::move(boundaries);
  env.unknowns = std::move(unknowns);
  env.evidence = std::move(evidence);
  env.truncated = any_truncated;
  // A single truncation reason across all truncated sites is reported as-is;
  // only mixed reasons collapse to the generic per_sink_budget label. The
  // authoritative per-site facts live in result.sites[i].truncation.
  env.reason =
      any_truncated
          ? std::optional<std::string>(truncation_reasons.size() == 1
                                           ? *truncation_reasons.begin()
                                           : "per_sink_budget")
          : std::nullopt;
  env.visited = visited_total;
  env.budget = budget_nodes;
  llvm::json::Array section_list;
  for (const std::string &section : sections)
    section_list.push_back(section);
  env.diagnostics = llvm::json::Array{
      llvm::json::Object{
          {"kind", "composed_query"},
          {"scope", "function_local_operand_evidence"},
          {"sink", sink_kind},
          {"budget_nodes_per_sink", budget_nodes},
          {"budget_depth", budget_depth},
          {"detail", detail},
          {"sections", std::move(section_list)}},
      llvm::json::Object{
          {"kind", "truncation"},
          {"scope", "per_sink"},
          {"truncated_sites", truncated_sites},
          {"total_visited_nodes", visited_total}}};
  return env.build();
}

} // namespace irez
