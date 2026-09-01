// Standalone JSONL adapter binary, kept for debugging and for cross-checking
// the in-process adapter path used by `irez`. Same command-line contract as
// IREZ_V00_00: subcommands catalog/function/version, --input, --function-key,
// --format jsonl.
#include "adapter.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::SubCommand Catalog("catalog", "Emit module and function catalog");
static cl::SubCommand FunctionGraph("function", "Emit one function graph");
static cl::SubCommand Version("version", "Print adapter version");
static cl::opt<std::string> Input("input", cl::Required, cl::sub(Catalog),
                                  cl::sub(FunctionGraph));
static cl::opt<std::string> FunctionKey("function-key", cl::sub(FunctionGraph));
static cl::opt<std::string> Format("format", cl::init("jsonl"), cl::sub(Catalog),
                                   cl::sub(FunctionGraph));

static bool has_error(const irez::adapter::Result &result, std::string *message) {
  for (const json::Object &record : result.records)
    if (record.getString("record") == "error") {
      if (message)
        *message = record.getString("message").value_or("").str();
      return true;
    }
  return false;
}

static void emit_records(irez::adapter::Result result) {
  for (json::Object &record : result.records)
    outs() << json::Value(std::move(record)) << '\n';
}

int main(int argc, char **argv) {
  InitLLVM Init(argc, argv);
  cl::ParseCommandLineOptions(argc, argv);
  if (Version) {
    outs() << json::Value(irez::adapter::version_info()) << '\n';
    return 0;
  }
  if (Format != "jsonl") {
    errs() << "only --format jsonl is supported\n";
    return 2;
  }
  if (Catalog) {
    auto result = irez::adapter::catalog(Input);
    const bool failed = has_error(result, nullptr);
    emit_records(std::move(result));
    return failed ? 4 : 0;
  }
  if (FunctionGraph) {
    auto result = irez::adapter::function_graph(Input, FunctionKey);
    std::string message;
    const bool failed = has_error(result, &message);
    // Decide the exit code before moving the records out.
    const int code =
        !failed ? 0 : (message == "function key not found" ? 3 : 4);
    emit_records(std::move(result));
    return code;
  }
  errs() << "expected a subcommand: catalog, function, or version\n";
  return 2;
}
