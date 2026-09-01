#include "db.h"

#include <SQLiteCpp/Statement.h>
#include <sqlite3.h>

#include "error.h"

namespace irez {

std::string Cell::as_string() const {
  if (const auto *text = std::get_if<std::string>(&value))
    return *text;
  if (const auto *integer = std::get_if<std::int64_t>(&value))
    return std::to_string(*integer);
  return "";
}

std::optional<std::string> Cell::as_opt_string() const {
  if (is_null())
    return std::nullopt;
  return as_string();
}

static void bind_params(SQLite::Statement &statement, const std::vector<Param> &params) {
  int index = 1;
  for (const Param &param : params) {
    if (const auto *text = std::get_if<std::string>(&param.value))
      statement.bind(index, *text);
    else if (const auto *integer = std::get_if<std::int64_t>(&param.value))
      statement.bind(index, static_cast<std::int64_t>(*integer));
    else
      statement.bind(index); // NULL
    ++index;
  }
}

std::vector<Row> query_all(SQLite::Database &db, const std::string &sql,
                           const std::vector<Param> &params) {
  try {
    SQLite::Statement statement(db, sql);
    bind_params(statement, params);
    std::vector<Row> rows;
    while (statement.executeStep()) {
      Row row;
      for (int i = 0; i < statement.getColumnCount(); ++i) {
        const char *name = statement.getColumnName(i);
        if (row.count(name)) // first occurrence wins, like sqlite3.Row
          continue;
        const SQLite::Column column = statement.getColumn(i);
        Cell cell;
        if (column.isNull())
          cell.value = std::monostate{};
        else if (column.getType() == SQLITE_INTEGER)
          cell.value = column.getInt64();
        else
          cell.value = column.getString();
        row.emplace(name, std::move(cell));
      }
      rows.push_back(std::move(row));
    }
    return rows;
  } catch (const SQLite::Exception &exc) {
    throw IrezError(std::string("sqlite error: ") + exc.what(), 5);
  }
}

std::optional<Row> query_one(SQLite::Database &db, const std::string &sql,
                             const std::vector<Param> &params) {
  std::vector<Row> rows = query_all(db, sql, params);
  if (rows.empty())
    return std::nullopt;
  return std::move(rows.front());
}

void exec(SQLite::Database &db, const std::string &sql,
          const std::vector<Param> &params) {
  try {
    SQLite::Statement statement(db, sql);
    bind_params(statement, params);
    while (statement.executeStep()) {
    }
  } catch (const SQLite::Exception &exc) {
    throw IrezError(std::string("sqlite error: ") + exc.what(), 5);
  }
}

llvm::json::Value cell_to_json(const Cell &cell) {
  if (const auto *text = std::get_if<std::string>(&cell.value))
    return llvm::json::Value(*text);
  if (const auto *integer = std::get_if<std::int64_t>(&cell.value))
    return llvm::json::Value(*integer);
  return llvm::json::Value(nullptr);
}

llvm::json::Object row_to_json(const Row &row) {
  llvm::json::Object object;
  for (const auto &[name, cell] : row)
    object[name] = cell_to_json(cell);
  return object;
}

} // namespace irez
