#include "ncurses_bench_rows.h"

#include <aiz/bench/factory.h>

#include "ncurses_probe.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aiz::ncurses {

void rebuildBenchRows(TuiState& state, const HardwareInfo& hw, unsigned int gpuCount) {
  state.benches.clear();
  state.benchRowTitles.clear();
  state.benchRowIsHeader.clear();
  state.benchResults.clear();

  std::vector<std::string> gpuNames;
#if defined(_WIN32)
  gpuNames.resize(gpuCount);
  for (unsigned int i = 0; i < gpuCount; ++i) gpuNames[i] = "GPU" + std::to_string(i);
#else
  gpuNames = parseGpuNames(hw, gpuCount);
#endif

  auto addHeader = [&](const std::string& title) {
    state.benches.push_back(nullptr);
    state.benchRowTitles.push_back(title);
    state.benchRowIsHeader.push_back(true);
    state.benchResults.emplace_back();
  };

  auto addBench = [&](std::unique_ptr<IBenchmark> b) {
    const std::string title = b ? b->name() : std::string("(null)");
    state.benches.push_back(std::move(b));
    state.benchRowTitles.push_back(title);
    state.benchRowIsHeader.push_back(false);
    state.benchResults.emplace_back();
  };

  for (unsigned int gi = 0; gi < gpuCount; ++gi) {
    std::string gpuName = "unknown";
    const std::size_t idx = static_cast<std::size_t>(gi);
    if (idx < gpuNames.size()) gpuName = gpuNames[idx];

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

    // Inference benchmarks: currently device-0 only.
    if (gi == 0) {
    }
  }

  // If no GPUs are detected, still group GPU inference benches under a GPU header.
  if (gpuCount == 0) {
    addHeader("GPU0 - (no GPU detected)");
  }

  addHeader("CPU0 - " + (hw.cpuName.empty() ? std::string("unknown") : hw.cpuName));
  addBench(makeOrtCpuMatMulBenchmark());
  addBench(makeOrtCpuMemoryBandwidthBenchmark());
}

}  // namespace aiz::ncurses
