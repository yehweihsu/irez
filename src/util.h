#pragma once

#include <string>

namespace irez {

// RFC 4122 version 4 UUID, lowercase hex with hyphens.
// Portable across Linux/Windows via std::random_device.
std::string uuid4();

// UTC timestamp in the same shape as Python's
// datetime.now(timezone.utc).isoformat(): "2026-08-02T08:33:35.123456+00:00".
std::string now_iso8601();

// Lowercased file extension including the dot (".ll"), or "" when absent.
std::string lower_suffix(const std::string &path);

} // namespace irez
