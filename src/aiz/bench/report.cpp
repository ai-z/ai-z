#include <aiz/bench/report.h>

#include <aiz/bench/bench.h>
#include <aiz/bench/factory.h>
#include <aiz/hw/hardware_info.h>
#include <aiz/metrics/nvidia_nvml.h>
#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aiz {
namespace {
struct BenchRowInternal {
  std::string title;
  bool isHeader = false;
  std::unique_ptr<IBenchmark> bench;
  std::string result;
};

std::string escapeHtml(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char ch : in) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

std::string formatTimestamp(std::time_t t, const char* fmt) {
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, fmt);
  return oss.str();
}

std::string resultPlaceholder(const std::string& name) {
  if (name.find("PCIe") != std::string::npos || name.find("PCI") != std::string::npos) return "-- GB/s";
  if (name.find("FP") != std::string::npos || name.find("FLOPS") != std::string::npos) return "-- GFLOPS";
  if (name.find("INT") != std::string::npos) return "-- GOPS";
  return "--";
}

std::string htmlFromResult(const std::string& text) {
  std::string esc = escapeHtml(text);
  std::string out;
  out.reserve(esc.size());
  for (char ch : esc) {
    if (ch == '\n') {
      out += "<br/>";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

static bool startsWithAscii(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> parseGpuNamesFromHw(const HardwareInfo& hw, unsigned int gpuCount) {
  std::vector<std::string> names;
  names.resize(gpuCount, std::string("unknown"));

  for (const auto& line : hw.perGpuLines) {
    if (line.empty() || std::isspace(static_cast<unsigned char>(line.front()))) continue;
    if (!startsWithAscii(line, "GPU")) continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string idxStr = line.substr(3, colon - 3);
    unsigned int idx = 0;
    bool ok = !idxStr.empty();
    for (char ch : idxStr) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) { ok = false; break; }
      idx = idx * 10 + static_cast<unsigned int>(ch - '0');
    }
    if (!ok || idx >= gpuCount) continue;
    std::string name = line.substr(colon + 1);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    if (!name.empty()) names[idx] = std::move(name);
  }

  if (gpuCount == 1 && !hw.gpuName.empty()) {
    names[0] = hw.gpuName;
  }

  return names;
}

std::vector<BenchRowInternal> buildBenchRows(const HardwareInfo& hw, unsigned int gpuCount) {
  std::vector<BenchRowInternal> rows;

  auto addHeader = [&](const std::string& title) {
    rows.push_back(BenchRowInternal{title, true, nullptr, {}});
  };

  auto addBench = [&](std::unique_ptr<IBenchmark> b) {
    const std::string title = b ? b->name() : std::string("(null)");
    rows.push_back(BenchRowInternal{title, false, std::move(b), {}});
  };

  const std::vector<std::string> gpuNames = parseGpuNamesFromHw(hw, gpuCount);

  for (unsigned int gi = 0; gi < gpuCount; ++gi) {
    const std::string gpuName = (gi < gpuNames.size()) ? gpuNames[gi] : std::string("unknown");
    addHeader("GPU" + std::to_string(gi) + " - " + gpuName);
    addBench(makeGpuCudaPcieBandwidthBenchmark(gi));
    addBench(makeGpuVulkanPcieBandwidthBenchmark(gi));
    addBench(makeGpuOpenclPcieBandwidthBenchmark(gi));
    addBench(makeGpuFp32BenchmarkVulkan(gi));
    addBench(makeGpuFp32BenchmarkOpencl(gi));
    addBench(makeGpuFp16Benchmark(gi));
    addBench(makeGpuFp32Benchmark(gi));
    addBench(makeGpuFp64Benchmark(gi));
    addBench(makeGpuInt4Benchmark(gi));
    addBench(makeGpuInt8Benchmark(gi));
  }

  if (gpuCount == 0) {
    addHeader("GPU0 - (no GPU detected)");
  }

  addHeader("CPU0 - " + (hw.cpuName.empty() ? std::string("unknown") : hw.cpuName));
  addBench(makeOrtCpuMatMulBenchmark());
  addBench(makeOrtCpuMemoryBandwidthBenchmark());

  return rows;
}

std::optional<unsigned int> detectGpuCount() {
  if (const auto n = nvmlGpuCount()) {
    if (*n > 0) return *n;
  }
#if defined(AI_Z_PLATFORM_LINUX)
  const unsigned int nSys = linuxGpuCount();
  if (nSys > 0) return nSys;
#endif
  return 0u;
}

std::optional<std::string> writeHtmlReport(const std::vector<BenchRowInternal>& rows,
                                           const std::vector<std::string>& hwLines) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t nowT = std::chrono::system_clock::to_time_t(now);
  const std::string stamp = formatTimestamp(nowT, "%Y%m%d-%H%M%S");
  const std::string stampHuman = formatTimestamp(nowT, "%Y-%m-%d %H:%M:%S");

  const char* home = std::getenv("HOME");
  const std::string homeDir = (home && *home) ? std::string(home) : std::string(".");
  const std::string outPath = homeDir + "/ai-z-bench-" + stamp + ".html";

  std::ofstream out(outPath, std::ios::out | std::ios::trunc);
  if (!out) return std::nullopt;

  out << "<!doctype html>\n";
  out << "<html lang=\"en\">\n<head>\n";
  out << "<meta charset=\"utf-8\"/>\n";
  out << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n";
  out << "<title>AI-Z Benchmark Report</title>\n";
  out << "<style>";
  out << "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,\"Helvetica Neue\",Arial,sans-serif;margin:24px;color:#111}";
  out << "h1{margin:0 0 8px 0}h2{margin-top:28px}";
  out << "table{border-collapse:collapse;width:100%}th,td{padding:8px 10px;border-bottom:1px solid #ddd;vertical-align:top}";
  out << "th{background:#f3f4f6;text-align:left}tr.section th{background:#e5e7eb}td.result{white-space:pre-wrap;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,\"Liberation Mono\",\"Courier New\",monospace}";
  out << "small{color:#555}";
  out << "</style>\n</head>\n<body>\n";
  out << "<h1>AI-Z Benchmark Report</h1>\n";
  out << "<small>Generated: " << escapeHtml(stampHuman) << "</small>\n";

  out << "<h2>Benchmarks</h2>\n";
  out << "<table>\n";
  out << "<tr><th>Benchmark</th><th>Result</th></tr>\n";

  for (const auto& row : rows) {
    if (row.isHeader) {
      out << "<tr class=\"section\"><th colspan=\"2\">" << escapeHtml(row.title) << "</th></tr>\n";
      continue;
    }

    std::string result = row.result.empty() ? resultPlaceholder(row.title) : row.result;
    out << "<tr><td>" << escapeHtml(row.title) << "</td><td class=\"result\">" << htmlFromResult(result)
        << "</td></tr>\n";
  }

  out << "</table>\n";

  out << "<h2>Hardware</h2>\n";
  if (hwLines.empty()) {
    out << "<p><em>No hardware information available.</em></p>\n";
  } else {
    out << "<pre>";
    for (const auto& line : hwLines) {
      out << escapeHtml(line) << "\n";
    }
    out << "</pre>\n";
  }

  out << "</body>\n</html>\n";
  out.flush();
  if (!out) return std::nullopt;

  return outPath;
}
}  // namespace

std::optional<BenchReport> runBenchmarksAndGenerateHtmlReport() {
  const HardwareInfo hw = HardwareInfo::probe();
  const unsigned int gpuCount = detectGpuCount().value_or(0u);

  std::vector<BenchRowInternal> rows = buildBenchRows(hw, gpuCount);

  for (auto& row : rows) {
    if (row.isHeader) continue;
    const BenchResult r = row.bench ? row.bench->run() : BenchResult{false, "(null benchmark)"};
    row.result = r.ok ? r.summary : ("FAIL: " + r.summary);
  }

  const std::vector<std::string> hwLines = hw.toLines();
  const auto path = writeHtmlReport(rows, hwLines);
  if (!path) return std::nullopt;

  BenchReport report;
  report.path = *path;
  report.hardwareLines = hwLines;
  report.rows.reserve(rows.size());
  for (const auto& row : rows) {
    report.rows.push_back(BenchReportRow{row.title, row.isHeader, row.result});
  }

  return report;
}

}  // namespace aiz
