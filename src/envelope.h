#pragma once

#include <optional>
#include <string>

#include "llvm/Support/JSON.h"

namespace irez {

// The V0 response envelope shared by the CLI and the MCP server. Field set
// and semantics are identical to IREZ_V00_00 (schema_version 1).
struct Envelope {
  std::string command;
  std::optional<std::string> investigation;
  std::optional<std::string> target;
  llvm::json::Value result = llvm::json::Object{};
  llvm::json::Value capabilities = llvm::json::Array{};
  llvm::json::Value boundaries = llvm::json::Array{};
  llvm::json::Value unknowns = llvm::json::Array{};
  bool truncated = false;
  std::optional<std::string> reason;
  std::int64_t visited = 0;
  std::int64_t budget = 100;
  llvm::json::Value evidence = llvm::json::Array{};
  llvm::json::Value expandable = llvm::json::Array{};
  llvm::json::Value diagnostics = llvm::json::Array{};

  llvm::json::Object build() const;
};

// Parse a JSON document; throws IrezError(5) on malformed input.
llvm::json::Value parse_json(const std::string &text, const char *context);

// Deep-copy a JSON value, converting non-owning StringRef leaves into owned
// strings. llvm::json::Value constructed from StringRef/const char* does NOT
// own the bytes; any value that outlives its source must go through this.
llvm::json::Value own_json(const llvm::json::Value &value);

// Serialize with two-space indentation (human- and jq-friendly).
std::string dump_json(const llvm::json::Value &value);

// Compact serialization without whitespace, for JSON stored inside SQLite
// TEXT columns.
std::string json_to_string(const llvm::json::Value &value);

} // namespace irez
