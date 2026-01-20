#include "ncurses_bench.h"

#include <aiz/bench/bench.h>

#include <cstddef>
#include <mutex>

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

}  // namespace aiz::ncurses
