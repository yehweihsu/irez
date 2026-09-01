#include "envelope.h"

#include "error.h"
#include "schema.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstdint>
#include <functional>

namespace irez {

llvm::json::Object Envelope::build() const {
  llvm::json::Object truncation;
  truncation["truncated"] = truncated;
  truncation["reason"] = reason ? llvm::json::Value(*reason) : llvm::json::Value(nullptr);
  truncation["visited_nodes"] = visited;
  truncation["budget_nodes"] = budget;

  llvm::json::Object out;
  // schema_version is retained as the V0 compatibility alias.
  out["schema_version"] = kApiSchemaVersion;
  out["api_schema_version"] = kApiSchemaVersion;
  out["command"] = command;
  out["investigation"] = investigation ? llvm::json::Value(*investigation)
                                       : llvm::json::Value(nullptr);
  out["target"] = target ? llvm::json::Value(*target) : llvm::json::Value(nullptr);
  out["result"] = result;
  out["capabilities_used"] = capabilities;
  out["boundaries"] = boundaries;
  out["unknowns"] = unknowns;
  out["truncation"] = std::move(truncation);
  out["evidence_refs"] = evidence;
  out["expandable"] = expandable;
  out["diagnostics"] = diagnostics;
  return out;
}

llvm::json::Value parse_json(const std::string &text, const char *context) {
  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
  if (!parsed)
    throw IrezError(llvm::formatv("invalid JSON in {0}: {1}", context,
                                  llvm::toString(parsed.takeError()))
                        .str(),
                    5);
  return std::move(*parsed);
}

llvm::json::Value own_json(const llvm::json::Value &value) {
  if (const llvm::json::Object *object = value.getAsObject()) {
    llvm::json::Object out;
    for (const auto &kv : *object)
      out[kv.first] = own_json(kv.second);
    return out;
  }
  if (const llvm::json::Array *array = value.getAsArray()) {
    llvm::json::Array out;
    for (const llvm::json::Value &element : *array)
      out.push_back(own_json(element));
    return out;
  }
  if (const std::optional<llvm::StringRef> text = value.getAsString())
    return llvm::json::Value(text->str()); // std::string -> owning Value
  return value; // null, booleans, and numbers are self-contained
}

std::string dump_json(const llvm::json::Value &value) {
  bool lossy = false;
  auto sanitize_text = [&lossy](llvm::StringRef input) {
    std::string output;
    output.reserve(input.size());
    const auto *bytes = reinterpret_cast<const unsigned char *>(input.data());
    for (std::size_t i = 0; i < input.size();) {
      const unsigned char lead = bytes[i];
      std::size_t width = 0;
      std::uint32_t codepoint = 0;
      if (lead <= 0x7f) {
        width = 1;
        codepoint = lead;
      } else if (lead >= 0xc2 && lead <= 0xdf) {
        width = 2;
        codepoint = lead & 0x1f;
      } else if (lead >= 0xe0 && lead <= 0xef) {
        width = 3;
        codepoint = lead & 0x0f;
      } else if (lead >= 0xf0 && lead <= 0xf4) {
        width = 4;
        codepoint = lead & 0x07;
      }
      bool valid = width != 0 && i + width <= input.size();
      for (std::size_t j = 1; valid && j < width; ++j) {
        if ((bytes[i + j] & 0xc0) != 0x80) {
          valid = false;
          break;
        }
        codepoint = (codepoint << 6) | (bytes[i + j] & 0x3f);
      }
      if (valid) {
        const bool overlong = (width == 2 && codepoint < 0x80) ||
                              (width == 3 && codepoint < 0x800) ||
                              (width == 4 && codepoint < 0x10000);
        valid = !overlong && codepoint <= 0x10ffff &&
                !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      }
      if (valid) {
        // llvm::json normalizes malformed input to U+FFFD when a Value is
        // constructed, before this final serializer sees the original bytes.
        // Preserve an explicit signal when that replacement reaches stdout.
        if (codepoint == 0xfffd)
          lossy = true;
        output.append(input.data() + i, width);
        i += width;
      } else {
        output.append("\xef\xbf\xbd");
        lossy = true;
        ++i;
      }
    }
    return output;
  };
  std::function<llvm::json::Value(const llvm::json::Value &)> sanitize;
  sanitize = [&](const llvm::json::Value &item) -> llvm::json::Value {
    if (const auto *object = item.getAsObject()) {
      llvm::json::Object clean;
      for (const auto &entry : *object)
        clean[sanitize_text(entry.first)] = sanitize(entry.second);
      return clean;
    }
    if (const auto *array = item.getAsArray()) {
      llvm::json::Array clean;
      for (const auto &entry : *array)
        clean.push_back(sanitize(entry));
      return clean;
    }
    if (const auto text = item.getAsString())
      return sanitize_text(*text);
    return item;
  };
  llvm::json::Value safe = sanitize(value);
  if (lossy) {
    if (auto *object = safe.getAsObject()) {
      (*object)["encoding_lossy"] = true;
      llvm::json::Array *diagnostics = object->getArray("diagnostics");
      if (!diagnostics) {
        (*object)["diagnostics"] = llvm::json::Array{};
        diagnostics = object->getArray("diagnostics");
      }
      diagnostics->push_back(llvm::json::Object{
          {"kind", "text_encoding"}, {"status", "lossy"},
          {"replacement", "U+FFFD"}});
    }
  }
  std::string text;
  llvm::raw_string_ostream os(text);
  os << llvm::formatv("{0:2}", safe);
  os.flush();
  return text;
}

std::string json_to_string(const llvm::json::Value &value) {
  // llvm::json normalizes malformed string leaves when Values are built; the
  // final stdout serializer additionally reports any replacement as lossy.
  std::string text;
  llvm::raw_string_ostream os(text);
  os << value;
  os.flush();
  return text;
}

} // namespace irez
