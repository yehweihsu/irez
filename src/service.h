#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "llvm/Support/JSON.h"

#include "db.h"
#include "store.h"

namespace irez {

// Application service shared by the CLI and (through the CLI process) the
// MCP server. Ported from IREZ_V00_00 service.py with the fixes recorded in
// docs/PROGRESS.md (B1-B9, B12).
class Service {
public:
  explicit Service(std::filesystem::path state_dir);

  llvm::json::Object init(const std::string &name);
  llvm::json::Object status();
  llvm::json::Object backup(const std::filesystem::path &destination);
  llvm::json::Object artifacts();
  llvm::json::Object capabilities(const std::optional<std::string> &artifact);
  llvm::json::Object functions(const std::optional<std::string> &artifact,
                               const std::optional<std::string> &match);
  llvm::json::Object ingest(const std::filesystem::path &path, const std::string &index,
                            bool refresh = false);
  llvm::json::Object reindex(const std::string &artifact_handle,
                             const std::string &index = "full");
  llvm::json::Object materialize(const std::string &function_handle,
                                 bool refresh = false);
  llvm::json::Object show(const std::string &handle);
  llvm::json::Object show(const std::string &handle, const std::string &view,
                          const std::optional<std::string> &kind,
                          std::int64_t budget);
  llvm::json::Object source(const std::string &handle);
  llvm::json::Object uses(const std::string &handle, std::int64_t budget = 100);
  llvm::json::Object slice(const std::string &handle, const std::string &direction,
                           const std::vector<std::string> &relations,
                           std::int64_t budget_nodes, std::int64_t budget_depth);
  llvm::json::Object graph(const std::string &handle, const std::string &direction,
                           std::int64_t budget, const std::string &format);
  llvm::json::Object guards(const std::string &handle, std::int64_t budget = 100);
  llvm::json::Object expand(const std::string &handle);
  llvm::json::Object context(const std::string &handle, std::int64_t budget = 100);
  llvm::json::Object trace_return(const std::string &function_handle,
                                  const std::string &return_selector,
                                  std::int64_t budget_nodes,
                                  std::int64_t budget_depth,
                                  const std::string &detail = "summary",
                                  const std::vector<std::string> &include = {});
  // Store-sink twin of trace_return: traces every store instruction's stored
  // value backward. This is the result channel of kernel-shaped functions
  // (XLA/Numba/Triton) whose returns are `ret void` / `ret ptr null`.
  llvm::json::Object trace_stores(const std::string &function_handle,
                                  std::int64_t budget_nodes,
                                  std::int64_t budget_depth,
                                  const std::string &detail = "summary",
                                  const std::vector<std::string> &include = {});

  // Resolved absolute state directory, forward slashes on every OS.
  std::string resolved_state_dir() const;

  Store &store() { return store_; }

private:
  Store store_;

  // The single investigation id; throws IrezError when state is missing.
  std::string investigation_id(SQLite::Database &db);
  // Resolve an artifact row by id, or the most recent one.
  Row artifact_or_throw(SQLite::Database &db,
                        const std::optional<std::string> &artifact_id);
  // Lazily materialize a catalog-only function handle (show/slice/uses/
  // guards/source all trigger this, not just show).
  void ensure_materialized(SQLite::Database &db, const std::string &handle);
  // Bounded-traversal cores on a caller-provided connection, so composed
  // queries (trace_return) share one connection across all return sites
  // instead of opening a fresh connection per slice/source call.
  llvm::json::Object slice_on(SQLite::Database &db, const std::string &handle,
                              const std::string &direction,
                              const std::vector<std::string> &relations,
                              std::int64_t budget_nodes,
                              std::int64_t budget_depth);
  // Shared bounded trace core behind trace_return / trace_stores: traces each
  // sink entity backward over llvm.operand and composes the per-site payload.
  // `sink_kind` is "return" or "store".
  llvm::json::Object run_trace(SQLite::Database &db, const Row &function,
                               std::vector<Row> sinks,
                               const std::string &sink_kind,
                               const std::string &command,
                               const std::set<std::string> &sections,
                               const std::string &detail,
                               std::int64_t budget_nodes,
                               std::int64_t budget_depth);
  llvm::json::Object source_on(SQLite::Database &db, const std::string &handle);
};

// Public handle for an entity: "irez:<hash16>:llvm:<kind>:<rest>".
std::string entity_id(const Row &artifact, const std::string &local_key);

// [artifact_id, *sorted(unique(run_ids))], skipping nulls.
llvm::json::Array evidence_refs(const std::string &artifact_id,
                                const std::vector<std::optional<std::string>> &run_ids);

// [{name,status,precision?}] for known relation kinds, sorted by kind.
llvm::json::Array relation_capabilities(const std::vector<std::string> &kinds);

// Flatten an adapter capabilities record's inner map into the standard
// capabilities_used list shape.
llvm::json::Array flatten_capabilities(const llvm::json::Object &capabilities_record);

} // namespace irez
