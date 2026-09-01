#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SQLiteCpp/Database.h>

#include "db.h"

namespace irez {

// Owns the investigation state directory and the SQLite database inside it.
// Owns schema migration and the cross-process SQLite connection policy.
class Store {
public:
  explicit Store(std::filesystem::path state_dir);

  const std::filesystem::path &state_dir_path() const { return state_dir_; }
  const std::filesystem::path &db_path() const { return db_path_; }

  // Open an existing database read/write with foreign keys and a bounded busy
  // wait.  Only initialize() is allowed to create the database.
  SQLite::Database connect(bool create = false);

  // Produce a transactionally consistent SQLite snapshot, including WAL data.
  void backup(const std::filesystem::path &destination);

  // Create the state directory, run migrations, and create (or reuse) the
  // single investigation. Returns the investigation id.
  std::string initialize(const std::string &name);

  // The single investigation row, or nullopt when the state is empty.
  std::optional<Row> investigation(SQLite::Database &db);

  // Copy `source` into the immutable artifact store and insert the artifact
  // row. Returns {artifact row, created}. Deduplicates on content SHA-256.
  std::pair<Row, bool> ingest_file(SQLite::Database &db,
                                   const std::filesystem::path &source);

  std::string run_start(SQLite::Database &db, const std::string &producer,
                        const std::string &artifact_id,
                        const std::vector<std::string> &invocation);
  void run_end(SQLite::Database &db, const std::string &run_id,
               const std::string &status,
               const std::optional<llvm::json::Value> &error = std::nullopt);

private:
  std::filesystem::path state_dir_;
  std::filesystem::path db_path_;
};

} // namespace irez
