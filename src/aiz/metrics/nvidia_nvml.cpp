#include <aiz/metrics/nvidia_nvml.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>

namespace aiz {
namespace {

// Minimal NVML ABI surface (avoid depending on nvml.h).
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;

constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_TEMPERATURE_GPU = 0;

struct nvmlUtilization_t {
  unsigned int gpu;
  unsigned int memory;
};

struct nvmlMemory_t {
  std::uint64_t total;
  std::uint64_t free;
  std::uint64_t used;
};

using nvmlInit_v2_t = nvmlReturn_t (*)();
using nvmlShutdown_t = nvmlReturn_t (*)();
using nvmlDeviceGetCount_v2_t = nvmlReturn_t (*)(unsigned int*);
using nvmlDeviceGetHandleByIndex_v2_t = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using nvmlDeviceGetUtilizationRates_t = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
using nvmlDeviceGetMemoryInfo_t = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
using nvmlDeviceGetPowerUsage_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*mW*/);
using nvmlDeviceGetTemperature_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int /*sensor*/, unsigned int* /*temp*/);
using nvmlDeviceGetPowerState_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*pstate*/);

struct NvmlApi {
#if defined(_WIN32)
  HMODULE lib = nullptr;
#else
  void* lib = nullptr;
#endif
  nvmlInit_v2_t nvmlInit_v2 = nullptr;
  nvmlShutdown_t nvmlShutdown = nullptr;
  nvmlDeviceGetCount_v2_t nvmlDeviceGetCount_v2 = nullptr;
  nvmlDeviceGetHandleByIndex_v2_t nvmlDeviceGetHandleByIndex_v2 = nullptr;
  nvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates = nullptr;
  nvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo = nullptr;
  nvmlDeviceGetPowerUsage_t nvmlDeviceGetPowerUsage = nullptr;
  nvmlDeviceGetTemperature_t nvmlDeviceGetTemperature = nullptr;
  nvmlDeviceGetPowerState_t nvmlDeviceGetPowerState = nullptr;

  bool ok() const {
    return lib && nvmlInit_v2 && nvmlShutdown && nvmlDeviceGetCount_v2 && nvmlDeviceGetHandleByIndex_v2 &&
        nvmlDeviceGetUtilizationRates && nvmlDeviceGetMemoryInfo && nvmlDeviceGetPowerUsage &&
        nvmlDeviceGetTemperature && nvmlDeviceGetPowerState;
  }
};

static void* loadSym(void* lib, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
  return dlsym(lib, name);
#endif
}

static NvmlApi& api() {
  static NvmlApi a;
  static bool attempted = false;
  if (attempted) return a;
  attempted = true;

  // Prefer SONAME.
#if defined(_WIN32)
  a.lib = LoadLibraryA("nvml.dll");
#else
  a.lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
  if (!a.lib) {
    a.lib = dlopen("libnvidia-ml.so", RTLD_LAZY);
  }
#endif
  if (!a.lib) return a;

  a.nvmlInit_v2 = reinterpret_cast<nvmlInit_v2_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlInit_v2"));
  a.nvmlShutdown = reinterpret_cast<nvmlShutdown_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlShutdown"));
  a.nvmlDeviceGetCount_v2 = reinterpret_cast<nvmlDeviceGetCount_v2_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetCount_v2"));
  a.nvmlDeviceGetHandleByIndex_v2 = reinterpret_cast<nvmlDeviceGetHandleByIndex_v2_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetHandleByIndex_v2"));
  a.nvmlDeviceGetUtilizationRates = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetUtilizationRates"));
  a.nvmlDeviceGetMemoryInfo = reinterpret_cast<nvmlDeviceGetMemoryInfo_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetMemoryInfo"));
  a.nvmlDeviceGetPowerUsage = reinterpret_cast<nvmlDeviceGetPowerUsage_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetPowerUsage"));
  a.nvmlDeviceGetTemperature = reinterpret_cast<nvmlDeviceGetTemperature_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetTemperature"));
  a.nvmlDeviceGetPowerState = reinterpret_cast<nvmlDeviceGetPowerState_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetPowerState"));

  if (!a.ok()) {
    // Keep lib open; just mark unusable.
    return a;
  }
  return a;
}

struct NvmlSession {
  NvmlSession() {
    NvmlApi& a = api();
    if (!a.ok()) return;
    inited = (a.nvmlInit_v2() == NVML_SUCCESS);
  }

  ~NvmlSession() {
    NvmlApi& a = api();
    if (!inited || !a.ok()) return;
    (void)a.nvmlShutdown();
  }

  bool inited = false;
};

static double bytesToGiB(std::uint64_t b) {
  return static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0);
}

static std::optional<unsigned int> parsePstateNumber(const std::string& p) {
  if (p.size() < 2) return std::nullopt;
  if (p[0] != 'P' && p[0] != 'p') return std::nullopt;
  unsigned int v = 0;
  bool any = false;
  for (std::size_t i = 1; i < p.size(); ++i) {
    const char c = p[i];
    if (c < '0' || c > '9') return std::nullopt;
    any = true;
    v = v * 10u + static_cast<unsigned int>(c - '0');
  }
  if (!any) return std::nullopt;
  return v;
}

static std::optional<NvmlTelemetry> readNvmlTelemetryWithSession(nvmlDevice_t dev, NvmlApi& a) {
  nvmlUtilization_t util{};
  nvmlMemory_t mem{};
  unsigned int mw = 0;
  unsigned int tc = 0;
  unsigned int ps = 0;

  if (a.nvmlDeviceGetUtilizationRates(dev, &util) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetMemoryInfo(dev, &mem) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetPowerUsage(dev, &mw) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &tc) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetPowerState(dev, &ps) != NVML_SUCCESS) return std::nullopt;

  NvmlTelemetry t;
  t.gpuUtilPct = static_cast<double>(util.gpu);
  t.memUsedGiB = bytesToGiB(mem.used);
  t.memTotalGiB = bytesToGiB(mem.total);
  t.powerWatts = static_cast<double>(mw) / 1000.0;
  t.tempC = static_cast<double>(tc);
  t.pstate = "P" + std::to_string(ps);
  return t;
}

}  // namespace

std::optional<unsigned int> nvmlGpuCount() {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  return count;
}

std::optional<NvmlTelemetry> readNvmlTelemetryForGpu(unsigned int index) {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0 || index >= count) return std::nullopt;

  nvmlDevice_t dev{};
  if (a.nvmlDeviceGetHandleByIndex_v2(index, &dev) != NVML_SUCCESS) return std::nullopt;
  return readNvmlTelemetryWithSession(dev, a);
}

std::optional<NvmlTelemetry> readNvmlTelemetry() {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0) return std::nullopt;

  NvmlTelemetry agg;
  double maxUtil = 0.0;
  double sumUsed = 0.0;
  double sumTotal = 0.0;
  double sumPower = 0.0;
  double maxTemp = 0.0;
  std::optional<unsigned int> bestP;

  for (unsigned int i = 0; i < count; ++i) {
    nvmlDevice_t dev{};
    if (a.nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;

    const auto t = readNvmlTelemetryWithSession(dev, a);
    if (!t) continue;

    maxUtil = std::max(maxUtil, t->gpuUtilPct);
    sumUsed += t->memUsedGiB;
    sumTotal += t->memTotalGiB;
    sumPower += t->powerWatts;
    maxTemp = std::max(maxTemp, t->tempC);

    if (const auto p = parsePstateNumber(t->pstate)) {
      if (!bestP || *p < *bestP) bestP = *p;
    }
  }

  if (sumTotal <= 0.0 && sumPower <= 0.0 && maxUtil <= 0.0) {
    // Nothing usable (e.g., all devices failed).
    return std::nullopt;
  }

  agg.gpuUtilPct = maxUtil;
  agg.memUsedGiB = sumUsed;
  agg.memTotalGiB = sumTotal;
  agg.powerWatts = sumPower;
  agg.tempC = maxTemp;
  agg.pstate = bestP ? ("P" + std::to_string(*bestP)) : std::string{};
  return agg;
}

}  // namespace aiz
