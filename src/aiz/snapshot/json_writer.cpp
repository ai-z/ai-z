// SPDX-License-Identifier: MIT
// Minimal JSON writer implementation

#include <aiz/snapshot/json_writer.h>

namespace aiz::json {

std::string escape(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 8);

  for (const char c : s) {
    switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control character: output as \u00XX
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }

  return result;
}

void ObjectBuilder::maybeComma() {
  if (!first_) {
    ss_ << ", ";
  }
  first_ = false;
}

void ObjectBuilder::addString(const std::string& key, const std::string& value) {
  maybeComma();
  ss_ << "\"" << escape(key) << "\": \"" << escape(value) << "\"";
}

void ObjectBuilder::addOptionalString(const std::string& key, const std::optional<std::string>& value) {
  if (value) {
    addString(key, *value);
  }
}

std::string ObjectBuilder::build() const {
  return "{" + ss_.str() + "}";
}

void ArrayBuilder::addRaw(const std::string& json) {
  if (!first_) {
    ss_ << ", ";
  }
  first_ = false;
  ss_ << json;
}

std::string ArrayBuilder::build() const {
  return "[" + ss_.str() + "]";
}

}  // namespace aiz::json
