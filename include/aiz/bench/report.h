#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aiz {

struct BenchReportRow {
  std::string title;
  bool isHeader = false;
  std::string result;
};

struct BenchReport {
  std::string path;
  std::vector<BenchReportRow> rows;
  std::vector<std::string> hardwareLines;
};

// Runs all benchmarks and writes an HTML report to ~/ai-z-bench-<timestamp>.html.
// Returns the report path and the rendered rows on success.
std::optional<BenchReport> runBenchmarksAndGenerateHtmlReport();

}  // namespace aiz
