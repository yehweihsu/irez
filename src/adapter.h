#pragma once

#include <string>
#include <vector>

#include "llvm/Support/JSON.h"

namespace irez::adapter {

// Records emitted by the adapter, in emission order. Parse failures and
// unknown function keys are reported as a {"record": "error"} entry.
struct Result {
  std::vector<llvm::json::Object> records;
};

// Module + function catalog for a .ll or .bc file.
Result catalog(const std::string &input_path);

// Full structural graph of one function, selected by ordinal key ("f1") or
// by (mangled) function name.
Result function_graph(const std::string &input_path, const std::string &function_key);

llvm::json::Object version_info();

} // namespace irez::adapter
