// SPDX-License-Identifier: MIT
// Minimal JSON writer for snapshot serialization

#pragma once

#include <optional>
#include <string>
#include <sstream>

namespace aiz::json {

/// Escape a string for JSON output (handles quotes, backslashes, control chars).
std::string escape(const std::string& s);

/// JSON object builder for convenient construction.
class ObjectBuilder {
public:
  /// Add a string field (always included).
  void addString(const std::string& key, const std::string& value);

  /// Add an optional string field (omitted if nullopt).
  void addOptionalString(const std::string& key, const std::optional<std::string>& value);

  /// Build the final JSON object string.
  std::string build() const;

private:
  std::ostringstream ss_;
  bool first_ = true;

  void maybeComma();
};

/// JSON array builder for convenient construction.
class ArrayBuilder {
public:
  /// Add a raw JSON value (object string, etc.).
  void addRaw(const std::string& json);

  /// Build the final JSON array string.
  std::string build() const;

private:
  std::ostringstream ss_;
  bool first_ = true;
};

}  // namespace aiz::json
