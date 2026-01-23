#include "ncurses_bench.h"

#include <aiz/bench/bench.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace aiz::ncurses {

void benchJoinIfDone(std::thread& benchThread, TuiState& state) {
  if (!benchThread.joinable()) return;

  bool runningNow = false;
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    runningNow = state.benchmarksRunning;
  }
  if (!runningNow) benchThread.join();
}

namespace {
bool benchIsRunningLocked(const TuiState& state) {
  return state.benchmarksRunning;
}

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

void runAllBenchmarksBlocking(TuiState& state) {
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    state.lastBenchResult = "Running all...";
    state.benchmarksRunning = true;
    state.runningBenchIndex = -1;
  }

  for (int row = 0; row < static_cast<int>(state.benches.size()); ++row) {
    if (row >= 0 && row < static_cast<int>(state.benchRowIsHeader.size()) &&
        state.benchRowIsHeader[static_cast<std::size_t>(row)]) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      state.runningBenchIndex = row;
    }

    auto& b = state.benches[static_cast<std::size_t>(row)];
    const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      if (static_cast<std::size_t>(row) < state.benchResults.size()) {
        state.benchResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    state.runningBenchIndex = -1;
    state.benchmarksRunning = false;
  }
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
}  // namespace

void benchHandleActivate(std::thread& benchThread, TuiState& state) {
  // Ignore activation while a run is in progress.
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    if (benchThread.joinable() && benchIsRunningLocked(state)) return;
  }

  // Join completed worker before starting a new one.
  {
    bool runningNow = false;
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      runningNow = state.benchmarksRunning;
    }
    if (benchThread.joinable() && !runningNow) benchThread.join();
  }

  if (state.benchmarksSel == 0) {
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      state.lastBenchResult = "Running all...";
      state.benchmarksRunning = true;
      state.runningBenchIndex = -1;
    }

    benchThread = std::thread([&]() {
      for (int row = 0; row < static_cast<int>(state.benches.size()); ++row) {
        if (row >= 0 && row < static_cast<int>(state.benchRowIsHeader.size()) &&
            state.benchRowIsHeader[static_cast<std::size_t>(row)]) {
          continue;
        }
        {
          std::lock_guard<std::mutex> lk(state.benchMutex);
          state.runningBenchIndex = row;
        }

        auto& b = state.benches[static_cast<std::size_t>(row)];
        const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

        {
          std::lock_guard<std::mutex> lk(state.benchMutex);
          if (static_cast<std::size_t>(row) < state.benchResults.size()) {
            state.benchResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
          }
        }
      }

      {
        std::lock_guard<std::mutex> lk(state.benchMutex);
        state.runningBenchIndex = -1;
        state.benchmarksRunning = false;
      }
    });

    return;
  }

  const int row = state.benchmarksSel - 1;
  if (row >= 0 && row < static_cast<int>(state.benches.size()) &&
      row < static_cast<int>(state.benchRowIsHeader.size()) &&
      !state.benchRowIsHeader[static_cast<std::size_t>(row)]) {
    {
      std::lock_guard<std::mutex> lk(state.benchMutex);
      state.lastBenchResult = "Running...";
      state.benchmarksRunning = true;
      state.runningBenchIndex = row;
    }

    benchThread = std::thread([&, row]() {
      auto& b = state.benches[static_cast<std::size_t>(row)];
      const BenchResult r = b ? b->run() : BenchResult{false, "(null benchmark)"};

      {
        std::lock_guard<std::mutex> lk(state.benchMutex);
        if (static_cast<std::size_t>(row) < state.benchResults.size()) {
          state.benchResults[static_cast<std::size_t>(row)] = r.ok ? r.summary : ("FAIL: " + r.summary);
        }
        state.runningBenchIndex = -1;
        state.benchmarksRunning = false;
      }
    });
  } else {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    state.lastBenchResult = "(not runnable)";
  }
}

void benchShutdown(std::thread& benchThread) {
  if (benchThread.joinable()) benchThread.join();
}

std::optional<std::string> benchGenerateHtmlReport(std::thread& benchThread, TuiState& state) {
  if (benchThread.joinable()) {
    benchThread.join();
  }

  bool anyResult = false;
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    for (const auto& r : state.benchResults) {
      if (!r.empty()) {
        anyResult = true;
        break;
      }
    }
  }

  if (!anyResult) {
    runAllBenchmarksBlocking(state);
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t nowT = std::chrono::system_clock::to_time_t(now);
  const std::string stamp = formatTimestamp(nowT, "%Y%m%d-%H%M%S");
  const std::string stampHuman = formatTimestamp(nowT, "%Y-%m-%d %H:%M:%S");

  const char* home = std::getenv("HOME");
  const std::string homeDir = (home && *home) ? std::string(home) : std::string(".");
  const std::string outPath = homeDir + "/ai-z-bench-" + stamp + ".html";

  std::vector<std::string> titles;
  std::vector<bool> isHeader;
  std::vector<std::string> results;
  std::vector<std::string> hwLines;
  {
    std::lock_guard<std::mutex> lk(state.benchMutex);
    titles = state.benchRowTitles;
    isHeader = state.benchRowIsHeader;
    results = state.benchResults;
    hwLines = state.hardwareLines;
  }

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

  const std::size_t rows = std::max(titles.size(), results.size());
  for (std::size_t i = 0; i < rows; ++i) {
    const bool hdr = (i < isHeader.size()) ? isHeader[i] : false;
    const std::string name = (i < titles.size()) ? titles[i] : std::string("(unknown)");
    if (hdr) {
      out << "<tr class=\"section\"><th colspan=\"2\">" << escapeHtml(name) << "</th></tr>\n";
      continue;
    }

    std::string result = (i < results.size()) ? results[i] : std::string{};
    if (result.empty()) result = resultPlaceholder(name);
    out << "<tr><td>" << escapeHtml(name) << "</td><td class=\"result\">" << htmlFromResult(result) << "</td></tr>\n";
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

  state.statusLine = std::string("REPORT SAVED: ") + outPath;
  return outPath;
}

}  // namespace aiz::ncurses
