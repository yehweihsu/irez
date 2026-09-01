#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace irez {

// Exit codes match the V00_00 CLI contract:
//   0 success (including honestly declared partial results)
//   2 CLI argument error
//   3 target or artifact not found
//   4 LLVM adapter failure
//   5 state/database/local IO failure
//   6 unsupported operation or artifact type
//   7 another process owns a live analysis claim
class IrezError : public std::runtime_error {
public:
  explicit IrezError(const std::string &message, int code = 5,
                     std::string kind = "state_error")
      : std::runtime_error(message), exit_code(code), error_kind(std::move(kind)) {}
  int exit_code;
  std::string error_kind;
  // Optional per-command usage line attached by the CLI dispatcher to exit-2
  // (argument) errors, so a parse failure points at the correct syntax
  // instead of only naming the unexpected token.
  std::string usage;
};

class NotFound : public IrezError {
public:
  explicit NotFound(const std::string &message)
      : IrezError(message, 3, "not_found") {}
};

class AdapterFailure : public IrezError {
public:
  explicit AdapterFailure(const std::string &message)
      : IrezError(message, 4, "adapter_failure") {}
};

class Unsupported : public IrezError {
public:
  explicit Unsupported(const std::string &message)
      : IrezError(message, 6, "unsupported") {}
};

class Busy : public IrezError {
public:
  explicit Busy(const std::string &message)
      : IrezError(message, 7, "analysis_in_progress") {}
};

} // namespace irez
