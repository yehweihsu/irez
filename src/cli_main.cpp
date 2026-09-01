// irez: human-oriented JSON CLI over the IREZ core. Same command set and exit
// codes as the IREZ_V00_00 Python CLI; the LLVM adapter is linked in-process,
// so there is no --adapter option anymore.
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "envelope.h"
#include "error.h"
#include "schema.h"
#include "service.h"

namespace {

constexpr const char *kUsage = R"USAGE(usage: irez [--state-dir DIR] COMMAND ...

commands:
  init --name NAME
  status
  backup PATH
  capabilities [--artifact ID]
  ingest llvm PATH [--index catalog|full] [--refresh]
  reindex ARTIFACT [--index catalog|full]
  artifacts
  functions [--artifact ID] [--match REGEX]
  materialize function HANDLE
  show HANDLE [--view summary|exact|children]
              [--kind block|return|call] [--budget-nodes N]
  source HANDLE
  uses HANDLE [--budget-nodes N]
  slice HANDLE [--direction backward|forward] [--relations LIST]
               [--budget-nodes N] [--budget-depth N]
  graph HANDLE [--view value] [--direction backward|forward]
               [--budget-nodes N] [--format json|exact-ir]
  guards HANDLE [--budget-nodes N]
  expand HANDLE
  context HANDLE [--budget-nodes N]
  trace-return FUNCTION [--return all|HANDLE]
              [--budget-nodes N] [--budget-depth N]
              [--detail summary|graph] [--include chain,calls,source,nodes,relations,boundaries,flags]
  trace-stores FUNCTION [--budget-nodes N] [--budget-depth N]
              [--detail summary|graph] [--include chain,calls,source,nodes,relations,boundaries,flags]

global options must precede the command. The state directory defaults to
IREZ_STATE_DIR, or .irez when unset.
)USAGE";

// Per-command usage line attached to exit-2 (argument) errors (backlog F3):
// a bare "expected --index, got full" pointed at the option spelling when the
// real mistake was argument order. The error JSON now carries the correct
// syntax for the command that was being parsed.
const char *command_usage(const std::string &command) {
  static const std::map<std::string, const char *> lines = {
      {"init", "usage: irez init --name NAME"},
      {"status", "usage: irez status"},
      {"backup", "usage: irez backup PATH"},
      {"capabilities", "usage: irez capabilities [--artifact ID]"},
      {"ingest", "usage: irez ingest llvm PATH [--index catalog|full] [--refresh]"},
      {"reindex", "usage: irez reindex ARTIFACT [--index catalog|full]"},
      {"artifacts", "usage: irez artifacts"},
      {"functions", "usage: irez functions [--artifact ID] [--match REGEX]"},
      {"materialize", "usage: irez materialize function HANDLE"},
      {"show", "usage: irez show HANDLE [--view summary|exact|children] "
               "[--kind block|return|call] [--budget-nodes N]"},
      {"source", "usage: irez source HANDLE"},
      {"uses", "usage: irez uses HANDLE [--budget-nodes N]"},
      {"slice", "usage: irez slice HANDLE [--direction backward|forward] "
                "[--relations LIST] [--budget-nodes N] [--budget-depth N]"},
      {"graph", "usage: irez graph HANDLE [--view value] "
                "[--direction backward|forward] [--budget-nodes N] "
                "[--format json|exact-ir]"},
      {"guards", "usage: irez guards HANDLE [--budget-nodes N]"},
      {"expand", "usage: irez expand HANDLE"},
      {"context", "usage: irez context HANDLE [--budget-nodes N]"},
      {"trace-return", "usage: irez trace-return FUNCTION [--return all|HANDLE] "
                       "[--budget-nodes N] [--budget-depth N] "
                       "[--detail summary|graph] [--include LIST]"},
      {"trace-stores", "usage: irez trace-stores FUNCTION [--budget-nodes N] "
                       "[--budget-depth N] [--detail summary|graph] "
                       "[--include LIST]"},
  };
  const auto it = lines.find(command);
  return it == lines.end() ? nullptr : it->second;
}

struct Args {
  std::vector<std::string> items;
  std::size_t pos = 0;

  bool empty() const { return pos >= items.size(); }
  std::string peek() const { return empty() ? "" : items[pos]; }
  std::string take() {
    if (empty())
      throw irez::IrezError("missing argument", 2);
    return items[pos++];
  }
  // Take the value of --name VALUE or --name=VALUE.
  std::string option_value(const std::string &name) {
    std::string current = take();
    const std::string with_equals = name + "=";
    if (current.rfind(with_equals, 0) == 0)
      return current.substr(with_equals.size());
    if (current != name)
      throw irez::IrezError("expected " + name + ", got " + current, 2);
    return take();
  }
  bool consume(const std::string &name) {
    if (peek() == name) {
      ++pos;
      return true;
    }
    return false;
  }
  std::int64_t integer(const std::string &name) {
    const std::string text = option_value(name);
    try {
      return std::stoll(text);
    } catch (const std::exception &) {
      throw irez::IrezError("invalid integer for " + name + ": " + text, 2);
    }
  }
};

std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

void require_choice(const std::string &name, const std::string &value,
                    std::initializer_list<const char *> choices) {
  for (const char *choice : choices)
    if (value == choice)
      return;
  throw irez::IrezError("invalid value for " + name + ": " + value, 2);
}

void require_nonnegative(const std::string &name, std::int64_t value) {
  if (value < 0)
    throw irez::IrezError(name + " must be non-negative", 2);
}

void require_end(const Args &args) {
  if (!args.empty())
    throw irez::IrezError("unexpected argument: " + args.peek(), 2);
}

int usage_error(const std::string &message) {
  if (!message.empty())
    std::cerr << "error: " << message << '\n';
  std::cerr << kUsage;
  return 2;
}

int run(int argc, char **argv) {
  Args args;
  for (int i = 1; i < argc; ++i)
    args.items.emplace_back(argv[i]);

  std::string state_dir = ".irez";
  if (const char *env = std::getenv("IREZ_STATE_DIR"))
    state_dir = env;

  // Global options precede the command, mirroring the Python CLI.
  while (!args.empty() && args.peek().rfind("--", 0) == 0) {
    if (args.peek() == "--state-dir" || args.peek().rfind("--state-dir=", 0) == 0)
      state_dir = args.option_value("--state-dir");
    else if (args.peek() == "--help" || args.peek() == "-h") {
      std::cout << kUsage;
      return 0;
    } else {
      return usage_error("unknown global option: " + args.peek());
    }
  }
  if (args.empty())
    return usage_error("missing command");

  const std::string command = args.take();
  irez::Service service(state_dir);
  llvm::json::Object response;

  try {
  if (command == "init") {
    std::optional<std::string> name;
    while (!args.empty())
      name = args.option_value("--name");
    if (!name)
      return usage_error("init requires --name");
    response = service.init(*name);
  } else if (command == "status") {
    require_end(args);
    response = service.status();
  } else if (command == "backup") {
    const std::string destination = args.take();
    require_end(args);
    response = service.backup(destination);
  } else if (command == "capabilities") {
    std::optional<std::string> artifact;
    while (!args.empty())
      artifact = args.option_value("--artifact");
    response = service.capabilities(artifact);
  } else if (command == "ingest") {
    if (args.take() != "llvm")
      return usage_error("ingest requires the llvm subcommand");
    const std::string path = args.take();
    std::string index = "catalog";
    bool refresh = false;
    while (!args.empty()) {
      if (args.consume("--refresh"))
        refresh = true;
      else
        index = args.option_value("--index");
    }
    require_choice("--index", index, {"catalog", "full"});
    response = service.ingest(path, index, refresh);
  } else if (command == "reindex") {
    const std::string artifact = args.take();
    std::string index = "full";
    while (!args.empty())
      index = args.option_value("--index");
    require_choice("--index", index, {"catalog", "full"});
    response = service.reindex(artifact, index);
  } else if (command == "artifacts") {
    require_end(args);
    response = service.artifacts();
  } else if (command == "functions") {
    std::optional<std::string> artifact, match;
    while (!args.empty()) {
      if (args.peek() == "--artifact" || args.peek().rfind("--artifact=", 0) == 0)
        artifact = args.option_value("--artifact");
      else
        match = args.option_value("--match");
    }
    response = service.functions(artifact, match);
  } else if (command == "materialize") {
    if (args.take() != "function")
      return usage_error("materialize requires the function subcommand");
    const std::string handle = args.take();
    require_end(args);
    response = service.materialize(handle);
  } else if (command == "show") {
    const std::string handle = args.take();
    std::string view = "summary";
    std::optional<std::string> kind;
    std::int64_t budget = 100;
    while (!args.empty()) {
      if (args.peek() == "--view" || args.peek().rfind("--view=", 0) == 0)
        view = args.option_value("--view");
      else if (args.peek() == "--kind" || args.peek().rfind("--kind=", 0) == 0)
        kind = args.option_value("--kind");
      else if (args.peek() == "--budget-nodes" ||
               args.peek().rfind("--budget-nodes=", 0) == 0)
        budget = args.integer("--budget-nodes");
      else
        throw irez::IrezError("unknown show option: " + args.peek(), 2);
    }
    require_choice("--view", view, {"summary", "exact", "children"});
    if (kind)
      require_choice("--kind", *kind, {"block", "return", "call"});
    require_nonnegative("--budget-nodes", budget);
    response = service.show(handle, view, kind, budget);
  } else if (command == "source" || command == "expand") {
    const std::string handle = args.take();
    require_end(args);
    response = command == "source" ? service.source(handle) : service.expand(handle);
  } else if (command == "uses" || command == "guards" || command == "context") {
    const std::string handle = args.take();
    std::int64_t budget = 100;
    while (!args.empty())
      budget = args.integer("--budget-nodes");
    require_nonnegative("--budget-nodes", budget);
    if (command == "uses")
      response = service.uses(handle, budget);
    else if (command == "guards")
      response = service.guards(handle, budget);
    else
      response = service.context(handle, budget);
  } else if (command == "slice") {
    const std::string handle = args.take();
    std::string direction = "backward";
    std::string relations = "operand,control";
    std::int64_t budget_nodes = 100, budget_depth = 12;
    while (!args.empty()) {
      if (args.peek() == "--direction" || args.peek().rfind("--direction=", 0) == 0)
        direction = args.option_value("--direction");
      else if (args.peek() == "--relations" || args.peek().rfind("--relations=", 0) == 0)
        relations = args.option_value("--relations");
      else if (args.peek() == "--budget-nodes" ||
               args.peek().rfind("--budget-nodes=", 0) == 0)
        budget_nodes = args.integer("--budget-nodes");
      else if (args.peek() == "--budget-depth" ||
               args.peek().rfind("--budget-depth=", 0) == 0)
        budget_depth = args.integer("--budget-depth");
      else
        throw irez::IrezError("unknown slice option: " + args.peek(), 2);
    }
    require_choice("--direction", direction, {"backward", "forward"});
    require_nonnegative("--budget-nodes", budget_nodes);
    require_nonnegative("--budget-depth", budget_depth);
    response = service.slice(handle, direction, split(relations, ','), budget_nodes,
                             budget_depth);
  } else if (command == "trace-return") {
    const std::string handle = args.take();
    std::string selector = "all";
    std::string detail = "summary";
    std::string include;
    std::int64_t budget_nodes = 50;
    std::int64_t budget_depth = 8;
    while (!args.empty()) {
      if (args.peek() == "--return" || args.peek().rfind("--return=", 0) == 0)
        selector = args.option_value("--return");
      else if (args.peek() == "--detail" || args.peek().rfind("--detail=", 0) == 0)
        detail = args.option_value("--detail");
      else if (args.peek() == "--include" || args.peek().rfind("--include=", 0) == 0)
        include = args.option_value("--include");
      else if (args.peek() == "--budget-nodes" ||
               args.peek().rfind("--budget-nodes=", 0) == 0)
        budget_nodes = args.integer("--budget-nodes");
      else if (args.peek() == "--budget-depth" ||
               args.peek().rfind("--budget-depth=", 0) == 0)
        budget_depth = args.integer("--budget-depth");
      else
        throw irez::IrezError("unknown trace-return option: " + args.peek(), 2);
    }
    require_nonnegative("--budget-nodes", budget_nodes);
    require_nonnegative("--budget-depth", budget_depth);
    require_choice("--detail", detail, {"graph", "summary"});
    std::vector<std::string> sections;
    if (!include.empty())
      sections = split(include, ',');
    response = service.trace_return(handle, selector, budget_nodes, budget_depth,
                                    detail, sections);
  } else if (command == "trace-stores") {
    const std::string handle = args.take();
    std::string detail = "summary";
    std::string include;
    std::int64_t budget_nodes = 50;
    std::int64_t budget_depth = 8;
    while (!args.empty()) {
      if (args.peek() == "--detail" || args.peek().rfind("--detail=", 0) == 0)
        detail = args.option_value("--detail");
      else if (args.peek() == "--include" || args.peek().rfind("--include=", 0) == 0)
        include = args.option_value("--include");
      else if (args.peek() == "--budget-nodes" ||
               args.peek().rfind("--budget-nodes=", 0) == 0)
        budget_nodes = args.integer("--budget-nodes");
      else if (args.peek() == "--budget-depth" ||
               args.peek().rfind("--budget-depth=", 0) == 0)
        budget_depth = args.integer("--budget-depth");
      else
        throw irez::IrezError("unknown trace-stores option: " + args.peek(), 2);
    }
    require_nonnegative("--budget-nodes", budget_nodes);
    require_nonnegative("--budget-depth", budget_depth);
    require_choice("--detail", detail, {"graph", "summary"});
    std::vector<std::string> sections;
    if (!include.empty())
      sections = split(include, ',');
    response = service.trace_stores(handle, budget_nodes, budget_depth, detail,
                                    sections);
  } else if (command == "graph") {
    const std::string handle = args.take();
    std::string direction = "backward", format = "json";
    std::int64_t budget = 100;
    while (!args.empty()) {
      if (args.peek() == "--direction" || args.peek().rfind("--direction=", 0) == 0)
        direction = args.option_value("--direction");
      else if (args.peek() == "--format" || args.peek().rfind("--format=", 0) == 0)
        format = args.option_value("--format");
      else if (args.peek() == "--view" || args.peek().rfind("--view=", 0) == 0) {
        const std::string view = args.option_value("--view");
        require_choice("--view", view, {"value"});
      }
      else
        budget = args.integer("--budget-nodes");
    }
    require_choice("--direction", direction, {"backward", "forward"});
    require_choice("--format", format, {"json", "exact-ir"});
    require_nonnegative("--budget-nodes", budget);
    response = service.graph(handle, direction, budget, format);
  } else {
    return usage_error("unknown command: " + command);
  }
  } catch (irez::IrezError &exc) {
    if (exc.exit_code == 2 && exc.usage.empty())
      if (const char *line = command_usage(command))
        exc.usage = line;
    throw;
  }

  std::cout << irez::dump_json(llvm::json::Value(std::move(response))) << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const irez::IrezError &exc) {
    llvm::json::Object error;
    error["schema_version"] = irez::kApiSchemaVersion;
    error["api_schema_version"] = irez::kApiSchemaVersion;
    error["error"] = exc.what();
    error["error_kind"] = exc.error_kind;
    error["exit_code"] = static_cast<std::int64_t>(exc.exit_code);
    if (!exc.usage.empty())
      error["usage"] = exc.usage;
    std::cerr << irez::dump_json(llvm::json::Value(std::move(error))) << '\n';
    return exc.exit_code;
  } catch (const std::exception &exc) {
    llvm::json::Object error;
    error["schema_version"] = irez::kApiSchemaVersion;
    error["api_schema_version"] = irez::kApiSchemaVersion;
    error["error"] = exc.what();
    error["error_kind"] = "internal_error";
    error["exit_code"] = 5;
    std::cerr << irez::dump_json(llvm::json::Value(std::move(error))) << '\n';
    return 5;
  }
}
