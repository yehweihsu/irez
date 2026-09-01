// GENERATED FROM contract.json BY scripts/generate_contract.py - DO NOT EDIT.
#pragma once

#include <cstddef>

namespace irez {

inline constexpr int kSchemaVersion = 2;
inline constexpr int kApiSchemaVersion = 1;
inline constexpr int kAnalysisSchemaVersion = 2;
inline constexpr int kAdapterVersion = 2;
inline constexpr const char *kAdapterId = "llvm-ir";
inline constexpr const char *kAdapterVersionString = "2";

// The exact capability set a fresh materialization must complete.
// The cache-validity check compares this set exactly; a foreign or
// partial manifest with the same count is stale, never current.
struct MaterializationCapability {
  const char *name;
  const char *precision;
};
inline constexpr MaterializationCapability kMaterializationCapabilities[] = {
    {"entity_index", "exact"},
    {"operand_graph", "exact"},
    {"cfg", "exact"},
    {"control_dependence", "exact"},
    {"source_mapping", "partial"},
    {"direct_calls", "exact"},
};
inline constexpr std::size_t kMaterializationCapabilityCount =
    sizeof(kMaterializationCapabilities) / sizeof(kMaterializationCapabilities[0]);

} // namespace irez
