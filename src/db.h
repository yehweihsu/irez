#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <SQLiteCpp/Database.h>

#include "llvm/Support/JSON.h"

namespace irez {

// A single SQL cell: NULL, INTEGER, or TEXT (the only column types the IREZ
// schema uses). Behaves like a Python sqlite3.Row value for a faithful port.
struct Cell {
  std::variant<std::monostate, std::int64_t, std::string> value;

  bool is_null() const { return std::holds_alternative<std::monostate>(value); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(value); }

  // Integer as int64; throws if the cell is NULL or TEXT.
  std::int64_t as_int() const { return std::get<std::int64_t>(value); }
  // Text; integers are stringified, NULL returns "".
  std::string as_string() const;
  // Nullable text access.
  std::optional<std::string> as_opt_string() const;
};

using Row = std::map<std::string, Cell>;

struct Param {
  std::variant<std::monostate, std::int64_t, std::string> value;
  Param(std::nullptr_t) : value(std::monostate{}) {}
  Param(int v) : value(static_cast<std::int64_t>(v)) {}
  Param(std::int64_t v) : value(v) {}
  Param(const char *v) : value(std::string(v)) {}
  Param(std::string v) : value(std::move(v)) {}
};

// Run a query and materialize all rows. Column-name lookup matches Python's
// sqlite3.Row semantics: the FIRST occurrence of a duplicated name wins.
std::vector<Row> query_all(SQLite::Database &db, const std::string &sql,
                           const std::vector<Param> &params = {});

// Run a query expected to return at most one row.
std::optional<Row> query_one(SQLite::Database &db, const std::string &sql,
                             const std::vector<Param> &params = {});

// Execute a statement that returns no rows (INSERT/UPDATE/DELETE/DDL).
void exec(SQLite::Database &db, const std::string &sql,
          const std::vector<Param> &params = {});

llvm::json::Value cell_to_json(const Cell &cell);
llvm::json::Object row_to_json(const Row &row);

} // namespace irez
