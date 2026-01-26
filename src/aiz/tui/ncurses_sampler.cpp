#include "ncurses_sampler.h"

#include <aiz/metrics/nvidia_nvml.h>
#if defined(AI_Z_PLATFORM_LINUX)
#include <aiz/metrics/linux_gpu_sysfs.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace aiz::ncurses {

#if defined(_WIN32)
namespace {

static bool windowsEnvFlagSet(const char* name) {
  if (!name) return false;
  if (const char* v = std::getenv(name)) {
    if (*v == '\0') return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
  }
  return false;
}

struct PdhQueryState {
  HQUERY query = nullptr;
  HCOUNTER counter = nullptr;
  bool initialized = false;
  bool primed = false;
  std::wstring path;
};

std::optional<NvmlPcieThroughput> readWindowsPcieThroughput() {
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return std::nullopt;
  static PdhQueryState rxState;
  static PdhQueryState txState;

  auto initCounter = [](PdhQueryState& state, const std::vector<std::wstring>& paths) -> bool {
    if (state.initialized) return state.query && state.counter;
    state.initialized = true;
    for (const auto& path : paths) {
      if (PdhOpenQueryW(nullptr, 0, &state.query) != ERROR_SUCCESS) continue;
      if (PdhAddEnglishCounterW(state.query, path.c_str(), 0, &state.counter) == ERROR_SUCCESS) {
        state.path = path;
        return true;
      }
      PdhCloseQuery(state.query);
      state.query = nullptr;
      state.counter = nullptr;
    }
    return false;
  };

  const std::vector<std::wstring> rxPaths = {
    L"\\PCI Express(*)\\Bytes Received/sec",
    L"\\PCI Express Root Port(*)\\Bytes Received/sec"
  };
  const std::vector<std::wstring> txPaths = {
    L"\\PCI Express(*)\\Bytes Sent/sec",
    L"\\PCI Express Root Port(*)\\Bytes Sent/sec"
  };

  if (!initCounter(rxState, rxPaths)) return std::nullopt;
  if (!initCounter(txState, txPaths)) return std::nullopt;

  if (PdhCollectQueryData(rxState.query) != ERROR_SUCCESS) return std::nullopt;
  if (PdhCollectQueryData(txState.query) != ERROR_SUCCESS) return std::nullopt;

  if (!rxState.primed || !txState.primed) {
    rxState.primed = true;
    txState.primed = true;
    return std::nullopt;
  }

  auto readSum = [](PdhQueryState& state) -> std::optional<double> {
    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) return std::nullopt;

    const std::size_t words = (static_cast<std::size_t>(bufferSize) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    std::vector<std::uint64_t> buffer(words);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    DWORD alignedBytes = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
    DWORD itemCount2 = itemCount;
    status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_LARGE, &alignedBytes, &itemCount2, items);
    if (status != ERROR_SUCCESS) return std::nullopt;

    double total = 0.0;
    for (DWORD i = 0; i < itemCount2; ++i) {
      total += static_cast<double>(items[i].FmtValue.largeValue);
    }
    return total;
  };

  const auto rxBytes = readSum(rxState);
  const auto txBytes = readSum(txState);
  if (!rxBytes || !txBytes) return std::nullopt;

  NvmlPcieThroughput out;
  out.rxMBps = *rxBytes / (1024.0 * 1024.0);
  out.txMBps = *txBytes / (1024.0 * 1024.0);
  return out;
}

bool hasWindowsPcieCounters() {
  if (windowsEnvFlagSet("AI_Z_DISABLE_PDH")) return false;
  static std::optional<bool> cached;
  if (cached.has_value()) return *cached;

  const std::vector<std::wstring> rxPaths = {
    L"\\PCI Express(*)\\Bytes Received/sec",
    L"\\PCI Express Root Port(*)\\Bytes Received/sec"
  };
  const std::vector<std::wstring> txPaths = {
    L"\\PCI Express(*)\\Bytes Sent/sec",
    L"\\PCI Express Root Port(*)\\Bytes Sent/sec"
  };

  auto hasAny = [](const std::vector<std::wstring>& paths) -> bool {
    for (const auto& path : paths) {
      HQUERY query = nullptr;
      HCOUNTER counter = nullptr;
      if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) continue;
      if (PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return true;
      }
      PdhCloseQuery(query);
    }
    return false;
  };

  const bool supported = hasAny(rxPaths) && hasAny(txPaths);
  cached = supported;
  return supported;
}

}  // namespace
#endif

GpuTelemetrySampler::GpuTelemetrySampler(unsigned int gpuCount, bool hasNvml)
    : gpuCount_(gpuCount), hasNvml_(hasNvml) {
  cachedGpu_.resize(gpuCount_);
}

GpuTelemetrySampler::~GpuTelemetrySampler() {
  stop();
}

void GpuTelemetrySampler::start() {
  if (thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this]() { run(); });
}

void GpuTelemetrySampler::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
}

void GpuTelemetrySampler::snapshot(std::vector<std::optional<GpuTelemetry>>& outGpu,
                                  std::optional<NvmlPcieThroughput>& outPcie) const {
  std::lock_guard<std::mutex> lk(mu_);
  outGpu = cachedGpu_;
  outPcie = cachedPcie_;
}

bool GpuTelemetrySampler::isPcieThroughputSupported() {
#if defined(_WIN32)
  return hasWindowsPcieCounters();
#else
  return true;
#endif
}

void GpuTelemetrySampler::run() {
#if defined(_WIN32)
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool coInited = SUCCEEDED(hrCo);
#endif

  while (!stop_.load()) {
    std::vector<std::optional<GpuTelemetry>> nextGpu;
    nextGpu.resize(gpuCount_);

    for (unsigned int i = 0; i < gpuCount_; ++i) {
      if (hasNvml_) {
        nextGpu[static_cast<std::size_t>(i)] = readGpuTelemetryPreferNvml(i);
      } else {
#if defined(AI_Z_PLATFORM_LINUX)
        // Avoid NVML wrapper overhead on non-NVIDIA systems.
        if (const auto lt = readLinuxGpuTelemetry(i)) {
          GpuTelemetry t;
          t.utilPct = lt->utilPct;
          t.vramUsedGiB = lt->vramUsedGiB;
          t.vramTotalGiB = lt->vramTotalGiB;
          t.watts = lt->watts;
          t.tempC = lt->tempC;
          t.pstate = lt->pstate;
          t.source = lt->source;
          nextGpu[static_cast<std::size_t>(i)] = t;
        }
#elif defined(_WIN32)
        nextGpu[static_cast<std::size_t>(i)] = readGpuTelemetryPreferNvml(i);
#endif
      }
    }

    std::optional<NvmlPcieThroughput> nextPcie = hasNvml_ ? readNvmlPcieThroughput() : std::nullopt;
  #if defined(_WIN32)
    if (!nextPcie) nextPcie = readWindowsPcieThroughput();
  #endif

    {
      std::lock_guard<std::mutex> lk(mu_);
      cachedGpu_ = std::move(nextGpu);
      cachedPcie_ = nextPcie;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

#if defined(_WIN32)
  if (coInited) {
    CoUninitialize();
  }
#endif
}

}  // namespace aiz::ncurses
