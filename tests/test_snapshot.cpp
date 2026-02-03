// SPDX-License-Identifier: MIT
// Unit tests for snapshot JSON output

#include "test_framework.h"
#include <aiz/snapshot/json_writer.h>

TEST_CASE("JSON escape handles special characters") {
  REQUIRE(aiz::json::escape("hello") == "hello");
  REQUIRE(aiz::json::escape("say \"hi\"") == "say \\\"hi\\\"");
  REQUIRE(aiz::json::escape("back\\slash") == "back\\\\slash");
  REQUIRE(aiz::json::escape("line\nbreak") == "line\\nbreak");
  REQUIRE(aiz::json::escape("tab\there") == "tab\\there");
}

TEST_CASE("ObjectBuilder produces valid JSON") {
  aiz::json::ObjectBuilder obj;
  obj.addString("name", "test");
  obj.addString("value", "123");
  
  std::string result = obj.build();
  REQUIRE(result.front() == '{');
  REQUIRE(result.back() == '}');
  REQUIRE(result.find("\"name\": \"test\"") != std::string::npos);
  REQUIRE(result.find("\"value\": \"123\"") != std::string::npos);
}

TEST_CASE("ObjectBuilder omits nullopt values") {
  aiz::json::ObjectBuilder obj;
  obj.addString("present", "yes");
  obj.addOptionalString("missing", std::nullopt);
  obj.addOptionalString("also_present", std::make_optional<std::string>("value"));
  
  std::string result = obj.build();
  REQUIRE(result.find("\"present\"") != std::string::npos);
  REQUIRE(result.find("\"missing\"") == std::string::npos);
  REQUIRE(result.find("\"also_present\"") != std::string::npos);
}

TEST_CASE("ArrayBuilder produces valid JSON array") {
  aiz::json::ArrayBuilder arr;
  arr.addRaw("{\"a\": 1}");
  arr.addRaw("{\"b\": 2}");
  
  std::string result = arr.build();
  REQUIRE(result.front() == '[');
  REQUIRE(result.back() == ']');
  REQUIRE(result.find("{\"a\": 1}") != std::string::npos);
  REQUIRE(result.find("{\"b\": 2}") != std::string::npos);
}

TEST_CASE("Device snapshot JSON serialization") {
  // Test building a device-like JSON object manually
  aiz::json::ObjectBuilder obj;
  obj.addString("device_type", "gpu");
  obj.addString("device_name", "Test GPU");
  obj.addOptionalString("gpu_util", std::make_optional<std::string>("50%"));
  obj.addOptionalString("temp", std::make_optional<std::string>("65C"));
  obj.addOptionalString("fan_speed", std::nullopt);  // Should be omitted
  
  std::string json = obj.build();
  REQUIRE(json.find("\"device_type\": \"gpu\"") != std::string::npos);
  REQUIRE(json.find("\"device_name\": \"Test GPU\"") != std::string::npos);
  REQUIRE(json.find("\"gpu_util\": \"50%\"") != std::string::npos);
  REQUIRE(json.find("\"temp\": \"65C\"") != std::string::npos);
  REQUIRE(json.find("fan_speed") == std::string::npos);  // Omitted
}

TEST_CASE("Snapshot-like JSON structure") {
  // Build a snapshot structure manually
  aiz::json::ArrayBuilder devicesArray;
  
  {
    aiz::json::ObjectBuilder dev;
    dev.addString("device_type", "cpu");
    dev.addString("device_name", "Test CPU");
    dev.addOptionalString("cpu_util", std::make_optional<std::string>("45%"));
    devicesArray.addRaw(dev.build());
  }
  
  {
    aiz::json::ObjectBuilder dev;
    dev.addString("device_type", "ram");
    dev.addString("device_name", "System Memory");
    dev.addOptionalString("ram_util", std::make_optional<std::string>("60%"));
    devicesArray.addRaw(dev.build());
  }
  
  std::string timestamp = "2026-02-01T12:00:00Z";
  std::string json = "{\"timestamp\": \"" + timestamp + "\", \"devices\": " + devicesArray.build() + "}";
  
  REQUIRE(json.find("\"timestamp\"") != std::string::npos);
  REQUIRE(json.find("2026-02-01T12:00:00Z") != std::string::npos);
  REQUIRE(json.find("\"devices\"") != std::string::npos);
  REQUIRE(json.find("\"device_type\": \"cpu\"") != std::string::npos);
  REQUIRE(json.find("\"device_type\": \"ram\"") != std::string::npos);
}

// Note: Tests for captureSystemSnapshot() and snapshotToJson() require the full
// aiz-core library. Run them via the main CLI: ai-z --snapshot
