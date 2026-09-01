#include "store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>
#include <sqlite3.h>

#include "error.h"
#include "envelope.h"
#include "schema.h"
#include "util.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

namespace irez {
namespace {

bool durable_sync(const std::filesystem::path &path) {
#ifdef _WIN32
  // FlushFileBuffers (and therefore _commit) requires a handle with write
  // access; a read-only descriptor makes _commit fail unconditionally, which
  // broke every artifact ingest on Windows.
  const int fd = _wopen(path.c_str(), _O_RDWR | _O_BINARY);
  if (fd < 0)
    return false;
  const bool ok = _commit(fd) == 0;
  _close(fd);
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
#endif
  return ok;
}

} // namespace

Store::Store(std::filesystem::path state_dir)
    : state_dir_(std::filesystem::absolute(state_dir).lexically_normal()),
      db_path_(state_dir_ / "investigation.sqlite") {}

SQLite::Database Store::connect(bool create) {
  try {
    const int flags = SQLite::OPEN_READWRITE | (create ? SQLite::OPEN_CREATE : 0);
    SQLite::Database db(db_path_.string(), flags);
    db.setBusyTimeout(10000);
    db.exec("PRAGMA foreign_keys = ON");
    // WAL lets MCP query processes proceed while a materialization commits.
    db.exec("PRAGMA journal_mode = WAL");
    db.exec("PRAGMA synchronous = FULL");
    if (!create) {
      auto schema = query_one(
          db, "SELECT value FROM schema_meta WHERE key='schema_version'");
      if (!schema)
        throw IrezError("state is not initialized; run `irez init`", 5);
      if ((*schema)["value"].as_string() != std::to_string(kSchemaVersion))
        throw IrezError("unsupported state schema version " +
                            (*schema)["value"].as_string() +
                            " (this binary requires " +
                            std::to_string(kSchemaVersion) +
                            "); create a new state and reindex the original artifacts",
                        5);
      const auto tables = query_one(
          db, "SELECT count(*) AS n FROM sqlite_master WHERE type='table' AND name IN "
              "('schema_meta','investigations','artifacts','analysis_runs','entities',"
              "'relations','source_locations','materializations',"
              "'materialization_capabilities')");
      if (!tables || tables->at("n").as_int() != 9)
        throw IrezError("state schema is incomplete or corrupt", 5);
    }
    return db;
  } catch (const SQLite::Exception &exc) {
    throw IrezError(std::string("cannot open state database: ") + exc.what(), 5);
  }
}

void Store::backup(const std::filesystem::path &destination) {
  if (destination.empty())
    throw IrezError("backup destination is empty", 2);
  std::filesystem::create_directories(std::filesystem::absolute(destination).parent_path());
  if (!std::filesystem::is_regular_file(db_path_))
    throw IrezError("state database does not exist", 5);
  SQLite::Database source(db_path_.string(), SQLite::OPEN_READONLY);
  source.setBusyTimeout(10000);
  sqlite3 *target = nullptr;
  if (sqlite3_open_v2(destination.string().c_str(), &target,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string message = target ? sqlite3_errmsg(target) : "cannot open destination";
    if (target)
      sqlite3_close(target);
    throw IrezError("cannot create state backup: " + message, 5);
  }
  sqlite3_backup *operation = sqlite3_backup_init(target, "main", source.getHandle(), "main");
  if (!operation) {
    const std::string message = sqlite3_errmsg(target);
    sqlite3_close(target);
    throw IrezError("cannot start state backup: " + message, 5);
  }
  const int result = sqlite3_backup_step(operation, -1);
  const int finish = sqlite3_backup_finish(operation);
  sqlite3_close(target);
  if (result != SQLITE_DONE || finish != SQLITE_OK)
    throw IrezError("state backup failed", 5);
}

std::string Store::initialize(const std::string &name) {
  std::error_code ec;
  for (const char *directory :
       {"artifacts/llvm", "artifacts/adapter", "artifacts/logs", "exports", "fixtures"}) {
    std::filesystem::create_directories(state_dir_ / directory, ec);
    if (ec)
      throw IrezError("cannot create state directory: " + ec.message(), 5);
  }
  SQLite::Database db = connect(true);
  SQLite::Transaction transaction(db);
  const bool has_schema =
      query_one(db, "SELECT name FROM sqlite_master WHERE type='table' AND "
                    "name='schema_meta'")
          .has_value();
  if (!has_schema) {
    db.exec(kMigration1);
    exec(db, "INSERT INTO schema_meta VALUES ('schema_version', ?)",
         {"1"});
    db.exec(kMigration2);
  }
  auto version_row =
      query_one(db, "SELECT value FROM schema_meta WHERE key='schema_version'");
  if (!version_row)
    throw IrezError("state database has no schema version", 5);
  int version = 0;
  try {
    version = std::stoi((*version_row)["value"].as_string());
  } catch (const std::exception &) {
    throw IrezError("state database has an invalid schema version", 5);
  }
  if (version != kSchemaVersion)
    throw IrezError("unsupported state schema version " + std::to_string(version) +
                        " (this binary requires " + std::to_string(kSchemaVersion) +
                        "); create a new state and reindex the original artifacts",
                    5);
  if (auto existing = query_one(db, "SELECT id FROM investigations LIMIT 1")) {
    const std::string id = (*existing)["id"].as_string();
    transaction.commit();
    return id;
  }
  const std::string id = "investigation:" + uuid4();
  exec(db, "INSERT INTO investigations(id,name,created_at) VALUES(?,?,?)",
       {id, name, now_iso8601()});
  transaction.commit();
  return id;
}

std::optional<Row> Store::investigation(SQLite::Database &db) {
  return query_one(db, "SELECT * FROM investigations LIMIT 1");
}

std::pair<Row, bool> Store::ingest_file(SQLite::Database &db,
                                        const std::filesystem::path &source) {
  auto buffer = llvm::MemoryBuffer::getFile(source.string());
  if (!buffer)
    throw IrezError("cannot read artifact: " + source.string(), 5);
  const std::string content = (*buffer)->getBuffer().str();
  const auto digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef(content));
  std::string hex = llvm::toHex(digest);
  // Python's hexdigest() is lowercase; handles must match across implementations.
  std::transform(hex.begin(), hex.end(), hex.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Media type uses the lowercased suffix (".BC" is bitcode too).
  const std::string suffix = lower_suffix(source.string());
  const std::string media =
      suffix == ".bc" ? "application/llvm-bitcode" : "text/llvm";

  auto inv = investigation(db);
  if (!inv)
    throw IrezError("investigation is missing", 5);
  const std::string inv_id = (*inv)["id"].as_string();

  if (auto existing = query_one(
          db, "SELECT * FROM artifacts WHERE investigation_id=? AND content_sha256=?",
          {inv_id, hex})) {
    const std::filesystem::path backing =
        state_dir_ / (*existing)["stored_path"].as_string();
    auto stored = llvm::MemoryBuffer::getFile(backing.string());
    if (!stored)
      throw IrezError("deduplicated artifact backing file is missing: " +
                          backing.string(),
                      5);
    const auto stored_digest = llvm::SHA256::hash(
        llvm::arrayRefFromStringRef((*stored)->getBuffer()));
    std::string stored_hex = llvm::toHex(stored_digest);
    std::transform(stored_hex.begin(), stored_hex.end(), stored_hex.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (stored_hex != hex)
      throw IrezError("deduplicated artifact backing file hash mismatch: " +
                          backing.string(),
                      5);
    return {std::move(*existing), false};
  }

  const std::string artifact_id = "artifact:" + hex.substr(0, 16);
  const std::filesystem::path relative =
      std::filesystem::path("artifacts/llvm") / (hex + suffix);
  if (auto collision = query_one(db, "SELECT content_sha256 FROM artifacts WHERE id=?",
                                 {artifact_id}))
    throw IrezError("artifact handle hash-prefix collision", 5);
  const std::filesystem::path destination = state_dir_ / relative;
  const std::filesystem::path temporary =
      destination.string() + ".tmp-" + uuid4();
  std::error_code ec;
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      throw IrezError("cannot create artifact temporary file", 5);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output)
      throw IrezError("cannot write artifact temporary file", 5);
  }
  if (!durable_sync(temporary)) {
    std::filesystem::remove(temporary);
    throw IrezError("cannot flush artifact temporary file", 5);
  }
  if (std::filesystem::exists(destination)) {
    auto prior = llvm::MemoryBuffer::getFile(destination.string());
    if (!prior || (*prior)->getBuffer() != content) {
      std::filesystem::remove(temporary);
      throw IrezError("artifact destination exists with different content", 5);
    }
    std::filesystem::remove(temporary);
  } else {
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
      std::filesystem::remove(temporary);
      throw IrezError("cannot atomically store artifact: " + ec.message(), 5);
    }
  }
#ifndef _WIN32
  if (!durable_sync(destination.parent_path()))
    throw IrezError("cannot flush artifact directory", 5);
#endif
  exec(db,
       "INSERT INTO artifacts "
       "(id,investigation_id,kind,content_sha256,stored_path,original_path,"
       " media_type,adapter_id,adapter_version,dialect_version) "
       "VALUES(?,?,?,?,?,?,?,?,?,?)",
       {artifact_id, inv_id, "llvm", hex, relative.generic_string(),
        std::filesystem::absolute(source).lexically_normal().string(), media,
        kAdapterId, kAdapterVersionString,
        std::string("llvm-") + LLVM_VERSION_STRING});
  auto row = query_one(db, "SELECT * FROM artifacts WHERE id=?", {artifact_id});
  return {std::move(*row), true};
}

std::string Store::run_start(SQLite::Database &db, const std::string &producer,
                             const std::string &artifact_id,
                             const std::vector<std::string> &invocation) {
  auto inv = investigation(db);
  if (!inv)
    throw IrezError("investigation is missing", 5);
  llvm::json::Array invocation_json;
  for (const std::string &argument : invocation)
    invocation_json.push_back(argument);
  llvm::json::Array inputs;
  inputs.push_back(artifact_id);
  llvm::json::Object configuration{
      {"analysis_schema_version", kAnalysisSchemaVersion},
      {"adapter_version", kAdapterVersion},
      {"llvm_build_version", LLVM_VERSION_STRING},
      {"llvm_ir_reader_version", LLVM_VERSION_STRING}};
  const std::string run_id = "run:" + uuid4();
  exec(db,
       "INSERT INTO analysis_runs VALUES(?,?,?,?,?,?,?,?,?,?,?)",
       {run_id, (*inv)["id"].as_string(), producer, kAdapterVersionString,
        json_to_string(llvm::json::Value(std::move(invocation_json))),
        json_to_string(llvm::json::Value(std::move(configuration))),
        json_to_string(llvm::json::Value(std::move(inputs))), now_iso8601(), nullptr,
        "running", nullptr});
  return run_id;
}

void Store::run_end(SQLite::Database &db, const std::string &run_id,
                    const std::string &status,
                    const std::optional<llvm::json::Value> &error) {
  std::optional<std::string> error_json;
  if (error)
    error_json = json_to_string(*error);
  exec(db, "UPDATE analysis_runs SET ended_at=?,status=?,error_json=? WHERE id=?",
       {now_iso8601(), status,
        error_json ? Param(*error_json) : Param(nullptr), run_id});
}

} // namespace irez
