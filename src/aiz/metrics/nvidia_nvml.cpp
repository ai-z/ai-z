#include <aiz/metrics/nvidia_nvml.h>

#include <dlfcn.h>
#if defined(__linux__)
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <vector>

namespace aiz {
namespace {

#if defined(__linux__)
// NVML can sometimes hang inside the driver stack. To keep the UI responsive while
// still using NVML, execute NVML queries in a child process we can time out + kill.
// This keeps NVML mandatory without relying on external tooling.
constexpr std::chrono::milliseconds kNvmlCallTimeout{700};

struct OptU32Msg {
  std::uint8_t has = 0;
  std::uint32_t v = 0;
};

struct OptTelemetryMsg {
  std::uint8_t has = 0;
  double gpuUtilPct = 0.0;
  double memUtilPct = 0.0;
  double memUsedGiB = 0.0;
  double memTotalGiB = 0.0;
  double powerWatts = 0.0;
  double tempC = 0.0;
  char pstate[16]{};

  std::uint32_t gpuClockMHz = 0;
  std::uint32_t memClockMHz = 0;
  std::uint32_t memTransferRateMHz = 0;
  std::uint32_t smMajor = 0;
  std::uint32_t smMinor = 0;

  double maxPowerLimitWatts = 0.0;
  std::uint32_t memBusWidthBits = 0;
  double maxMemBandwidthGBps = 0.0;
};

struct OptPcieThroughputMsg {
  std::uint8_t has = 0;
  NvmlPcieThroughput t{};
};

struct OptPcieLinkMsg {
  std::uint8_t has = 0;
  NvmlPcieLink l{};
};

struct OptStringMsg {
  std::uint8_t has = 0;
  std::uint16_t len = 0;
  char buf[192]{};
};

static bool readExact(int fd, void* out, std::size_t n) {
  std::uint8_t* p = reinterpret_cast<std::uint8_t*>(out);
  std::size_t got = 0;
  while (got < n) {
    const ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

static bool writeExact(int fd, const void* data, std::size_t n) {
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
  std::size_t sent = 0;
  while (sent < n) {
    const ssize_t r = ::write(fd, p + sent, n - sent);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    sent += static_cast<std::size_t>(r);
  }
  return true;
}

template <typename MsgT, typename Fn>
static std::optional<MsgT> callWithTimeout(Fn&& fn, std::chrono::milliseconds timeout) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return std::nullopt;

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return std::nullopt;
  }

  if (pid == 0) {
    // Child
    ::close(fds[0]);
    MsgT msg{};
    fn(msg);
    (void)writeExact(fds[1], &msg, sizeof(MsgT));
    ::close(fds[1]);
    _exit(0);
  }

  // Parent
  ::close(fds[1]);

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fds[0], &rfds);

  struct timeval tv;
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

  const int sel = ::select(fds[0] + 1, &rfds, nullptr, nullptr, &tv);
  if (sel <= 0) {
    // Timeout or error.
    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, nullptr, 0);
    ::close(fds[0]);
    return std::nullopt;
  }

  MsgT msg{};
  const bool ok = readExact(fds[0], &msg, sizeof(MsgT));
  ::close(fds[0]);
  (void)::waitpid(pid, nullptr, 0);
  if (!ok) return std::nullopt;
  return msg;
}
#endif

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
using nvmlDeviceGetPcieThroughput_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int /*counter*/, unsigned int* /*valueKBps*/);
using nvmlDeviceGetCurrPcieLinkGeneration_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*gen*/);
using nvmlDeviceGetCurrPcieLinkWidth_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*width*/);
using nvmlDeviceGetName_t = nvmlReturn_t (*)(nvmlDevice_t, char* /*name*/, unsigned int /*length*/);
using nvmlSystemGetNVMLVersion_t = nvmlReturn_t (*)(char* /*version*/, unsigned int /*length*/);
using nvmlSystemGetDriverVersion_t = nvmlReturn_t (*)(char* /*version*/, unsigned int /*length*/);
using nvmlDeviceGetClockInfo_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int /*clockType*/, unsigned int* /*clockMHz*/);
using nvmlDeviceGetMaxClockInfo_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int /*clockType*/, unsigned int* /*clockMHz*/);
using nvmlDeviceGetSupportedMemoryClocks_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*count*/, unsigned int* /*clocksMHz*/);
using nvmlDeviceGetSupportedGraphicsClocks_t = nvmlReturn_t (*) (
  nvmlDevice_t, unsigned int /*memoryClockMHz*/, unsigned int* /*count*/, unsigned int* /*clocksMHz*/);
using nvmlDeviceGetPerformanceModes_t = nvmlReturn_t (*)(nvmlDevice_t, char* /*perfModes*/, unsigned int /*length*/);
using nvmlDeviceGetCudaComputeCapability_t = nvmlReturn_t (*)(nvmlDevice_t, int* /*major*/, int* /*minor*/);
using nvmlDeviceGetPowerManagementLimitConstraints_t = nvmlReturn_t (*)(
  nvmlDevice_t, unsigned int* /*minLimitMilliWatts*/, unsigned int* /*maxLimitMilliWatts*/);
using nvmlDeviceGetMemoryBusWidth_t = nvmlReturn_t (*)(nvmlDevice_t, unsigned int* /*busWidthBits*/);

struct NvmlApi {
  void* lib = nullptr;
  nvmlInit_v2_t nvmlInit_v2 = nullptr;
  nvmlShutdown_t nvmlShutdown = nullptr;
  nvmlDeviceGetCount_v2_t nvmlDeviceGetCount_v2 = nullptr;
  nvmlDeviceGetHandleByIndex_v2_t nvmlDeviceGetHandleByIndex_v2 = nullptr;
  nvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates = nullptr;
  nvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo = nullptr;
  nvmlDeviceGetPowerUsage_t nvmlDeviceGetPowerUsage = nullptr;
  nvmlDeviceGetTemperature_t nvmlDeviceGetTemperature = nullptr;
  nvmlDeviceGetPowerState_t nvmlDeviceGetPowerState = nullptr;
  nvmlDeviceGetPcieThroughput_t nvmlDeviceGetPcieThroughput = nullptr;
  nvmlDeviceGetCurrPcieLinkGeneration_t nvmlDeviceGetCurrPcieLinkGeneration = nullptr;
  nvmlDeviceGetCurrPcieLinkWidth_t nvmlDeviceGetCurrPcieLinkWidth = nullptr;
  nvmlDeviceGetName_t nvmlDeviceGetName = nullptr;
  nvmlSystemGetNVMLVersion_t nvmlSystemGetNVMLVersion = nullptr;
  nvmlSystemGetDriverVersion_t nvmlSystemGetDriverVersion = nullptr;

  // Optional extras.
  nvmlDeviceGetClockInfo_t nvmlDeviceGetClockInfo = nullptr;
  nvmlDeviceGetMaxClockInfo_t nvmlDeviceGetMaxClockInfo = nullptr;
  nvmlDeviceGetSupportedMemoryClocks_t nvmlDeviceGetSupportedMemoryClocks = nullptr;
  nvmlDeviceGetSupportedGraphicsClocks_t nvmlDeviceGetSupportedGraphicsClocks = nullptr;
  nvmlDeviceGetPerformanceModes_t nvmlDeviceGetPerformanceModes = nullptr;
  nvmlDeviceGetCudaComputeCapability_t nvmlDeviceGetCudaComputeCapability = nullptr;
  nvmlDeviceGetPowerManagementLimitConstraints_t nvmlDeviceGetPowerManagementLimitConstraints = nullptr;
  nvmlDeviceGetMemoryBusWidth_t nvmlDeviceGetMemoryBusWidth = nullptr;

  bool ok() const {
    return lib && nvmlInit_v2 && nvmlShutdown && nvmlDeviceGetCount_v2 && nvmlDeviceGetHandleByIndex_v2 &&
        nvmlDeviceGetUtilizationRates && nvmlDeviceGetMemoryInfo && nvmlDeviceGetPowerUsage &&
        nvmlDeviceGetTemperature && nvmlDeviceGetPowerState;
  }
};

// Enum values from nvml.h (nvmlClockType_t).
constexpr unsigned int NVML_CLOCK_GRAPHICS = 0;
constexpr unsigned int NVML_CLOCK_MEM = 2;

// Counter values from nvml.h (nvmlPcieUtilCounter_t). Keep as integers to avoid including the header.
constexpr unsigned int NVML_PCIE_UTIL_TX_BYTES = 0;
constexpr unsigned int NVML_PCIE_UTIL_RX_BYTES = 1;

static void* loadSym(void* lib, const char* name) {
  return dlsym(lib, name);
}

static NvmlApi& api() {
  static NvmlApi a;
  static bool attempted = false;
  if (attempted) return a;
  attempted = true;

  // Prefer SONAME.
  a.lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
  if (!a.lib) {
    a.lib = dlopen("libnvidia-ml.so", RTLD_LAZY);
  }
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
  a.nvmlDeviceGetPcieThroughput = reinterpret_cast<nvmlDeviceGetPcieThroughput_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetPcieThroughput"));
  a.nvmlDeviceGetName = reinterpret_cast<nvmlDeviceGetName_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetName"));

  // Optional extras (best-effort).
  a.nvmlDeviceGetClockInfo = reinterpret_cast<nvmlDeviceGetClockInfo_t>(loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetClockInfo"));
    a.nvmlDeviceGetMaxClockInfo = reinterpret_cast<nvmlDeviceGetMaxClockInfo_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetMaxClockInfo"));
    a.nvmlDeviceGetSupportedMemoryClocks = reinterpret_cast<nvmlDeviceGetSupportedMemoryClocks_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetSupportedMemoryClocks"));
    a.nvmlDeviceGetSupportedGraphicsClocks = reinterpret_cast<nvmlDeviceGetSupportedGraphicsClocks_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetSupportedGraphicsClocks"));
    a.nvmlDeviceGetPerformanceModes = reinterpret_cast<nvmlDeviceGetPerformanceModes_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetPerformanceModes"));
  a.nvmlDeviceGetCudaComputeCapability = reinterpret_cast<nvmlDeviceGetCudaComputeCapability_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetCudaComputeCapability"));
    a.nvmlDeviceGetPowerManagementLimitConstraints = reinterpret_cast<nvmlDeviceGetPowerManagementLimitConstraints_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetPowerManagementLimitConstraints"));
    a.nvmlDeviceGetMemoryBusWidth = reinterpret_cast<nvmlDeviceGetMemoryBusWidth_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetMemoryBusWidth"));

    // Optional system queries.
    a.nvmlSystemGetNVMLVersion = reinterpret_cast<nvmlSystemGetNVMLVersion_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlSystemGetNVMLVersion"));
    a.nvmlSystemGetDriverVersion = reinterpret_cast<nvmlSystemGetDriverVersion_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlSystemGetDriverVersion"));

    // Optional (not required for a.ok()).
    a.nvmlDeviceGetCurrPcieLinkGeneration = reinterpret_cast<nvmlDeviceGetCurrPcieLinkGeneration_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetCurrPcieLinkGeneration"));
    a.nvmlDeviceGetCurrPcieLinkWidth = reinterpret_cast<nvmlDeviceGetCurrPcieLinkWidth_t>(
      loadSym(reinterpret_cast<void*>(a.lib), "nvmlDeviceGetCurrPcieLinkWidth"));

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

static std::optional<std::string> readNvmlGpuNameForGpuUnsafe(unsigned int index) {
  NvmlApi& a = api();
  if (!a.ok() || !a.nvmlDeviceGetName) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  nvmlDevice_t dev{};
  if (a.nvmlDeviceGetHandleByIndex_v2(index, &dev) != NVML_SUCCESS) return std::nullopt;

  char buf[128]{};
  if (a.nvmlDeviceGetName(dev, buf, static_cast<unsigned int>(sizeof(buf))) != NVML_SUCCESS) return std::nullopt;
  const std::string s(buf);
  if (s.empty()) return std::nullopt;
  return s;
}

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

static unsigned int parseMaxU32Token(const char* s, const char* key) {
  if (!s || !key) return 0;
  const std::size_t keyLen = std::strlen(key);
  unsigned int best = 0;

  const char* p = s;
  while ((p = std::strstr(p, key)) != nullptr) {
    p += keyLen;
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 10);
    if (end != p && v > best) best = static_cast<unsigned int>(v);
    if (!end || *end == '\0') break;
    p = end;
  }
  return best;
}

static std::optional<unsigned int> maxSupportedMemoryClockMHz(nvmlDevice_t dev, NvmlApi& a) {
  if (!a.nvmlDeviceGetSupportedMemoryClocks) return std::nullopt;

  unsigned int count = 0;
  (void)a.nvmlDeviceGetSupportedMemoryClocks(dev, &count, nullptr);
  if (count == 0) return std::nullopt;

  std::vector<unsigned int> clocks(count);
  if (a.nvmlDeviceGetSupportedMemoryClocks(dev, &count, clocks.data()) != NVML_SUCCESS || count == 0) return std::nullopt;

  unsigned int best = 0;
  for (unsigned int i = 0; i < count; ++i) best = std::max(best, clocks[i]);
  return (best > 0) ? std::optional<unsigned int>(best) : std::nullopt;
}

static std::optional<unsigned int> maxSupportedGraphicsClockMHz(nvmlDevice_t dev, NvmlApi& a) {
  if (!a.nvmlDeviceGetSupportedMemoryClocks || !a.nvmlDeviceGetSupportedGraphicsClocks) return std::nullopt;

  unsigned int memCount = 0;
  (void)a.nvmlDeviceGetSupportedMemoryClocks(dev, &memCount, nullptr);
  if (memCount == 0) return std::nullopt;

  std::vector<unsigned int> memClocks(memCount);
  if (a.nvmlDeviceGetSupportedMemoryClocks(dev, &memCount, memClocks.data()) != NVML_SUCCESS || memCount == 0) return std::nullopt;

  unsigned int best = 0;
  for (unsigned int i = 0; i < memCount; ++i) {
    const unsigned int memClock = memClocks[i];
    if (memClock == 0) continue;

    unsigned int gfxCount = 0;
    (void)a.nvmlDeviceGetSupportedGraphicsClocks(dev, memClock, &gfxCount, nullptr);
    if (gfxCount == 0) continue;

    std::vector<unsigned int> gfxClocks(gfxCount);
    if (a.nvmlDeviceGetSupportedGraphicsClocks(dev, memClock, &gfxCount, gfxClocks.data()) != NVML_SUCCESS || gfxCount == 0) {
      continue;
    }

    for (unsigned int j = 0; j < gfxCount; ++j) best = std::max(best, gfxClocks[j]);
  }

  return (best > 0) ? std::optional<unsigned int>(best) : std::nullopt;
}

static std::optional<unsigned int> maxMemoryTransferRateMHz(nvmlDevice_t dev, NvmlApi& a) {
  if (!a.nvmlDeviceGetPerformanceModes) return std::nullopt;

  // NVML returns a semicolon-separated list of perf levels, each with token=value pairs.
  // We prefer memtransferratemax when present (may be more comparable to spec figures).
  char buf[8192]{};
  if (a.nvmlDeviceGetPerformanceModes(dev, buf, static_cast<unsigned int>(sizeof(buf))) != NVML_SUCCESS) return std::nullopt;

  const unsigned int maxA = parseMaxU32Token(buf, "memtransferratemax=");
  const unsigned int maxB = parseMaxU32Token(buf, "memtransferrate=");
  const unsigned int best = std::max(maxA, maxB);
  return (best > 0) ? std::optional<unsigned int>(best) : std::nullopt;
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
  t.memUtilPct = static_cast<double>(util.memory);
  t.memUsedGiB = bytesToGiB(mem.used);
  t.memTotalGiB = bytesToGiB(mem.total);
  t.powerWatts = static_cast<double>(mw) / 1000.0;
  t.tempC = static_cast<double>(tc);
  t.pstate = "P" + std::to_string(ps);

  // Report MAX clocks (requested). Prefer supported clock tables (more stable/spec-like),
  // fall back to nvmlDeviceGetMaxClockInfo if needed.
  if (const auto mhz = maxSupportedGraphicsClockMHz(dev, a)) t.gpuClockMHz = *mhz;
  if (const auto mhz = maxSupportedMemoryClockMHz(dev, a)) t.memClockMHz = *mhz;

  if ((t.gpuClockMHz == 0 || t.memClockMHz == 0) && a.nvmlDeviceGetMaxClockInfo) {
    unsigned int mhz = 0;
    if (t.gpuClockMHz == 0 && a.nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_GRAPHICS, &mhz) == NVML_SUCCESS && mhz > 0) {
      t.gpuClockMHz = mhz;
    }

    mhz = 0;
    if (t.memClockMHz == 0 && a.nvmlDeviceGetMaxClockInfo(dev, NVML_CLOCK_MEM, &mhz) == NVML_SUCCESS && mhz > 0) {
      t.memClockMHz = mhz;
    }
  }

  if (const auto mt = maxMemoryTransferRateMHz(dev, a)) {
    t.memTransferRateMHz = *mt;
  }

  if (a.nvmlDeviceGetCudaComputeCapability) {
    int major = 0;
    int minor = 0;
    if (a.nvmlDeviceGetCudaComputeCapability(dev, &major, &minor) == NVML_SUCCESS && major > 0 && minor >= 0) {
      t.smMajor = static_cast<unsigned int>(major);
      t.smMinor = static_cast<unsigned int>(minor);
    }
  }

  // Max power limit (W) via constraints.
  if (a.nvmlDeviceGetPowerManagementLimitConstraints) {
    unsigned int minMw = 0;
    unsigned int maxMw = 0;
    if (a.nvmlDeviceGetPowerManagementLimitConstraints(dev, &minMw, &maxMw) == NVML_SUCCESS && maxMw > 0) {
      t.maxPowerLimitWatts = static_cast<double>(maxMw) / 1000.0;
    }
  }

  // Max memory bandwidth (GB/s) derived from max memory clock and bus width.
  if (a.nvmlDeviceGetMemoryBusWidth) {
    unsigned int bits = 0;
    if (a.nvmlDeviceGetMemoryBusWidth(dev, &bits) == NVML_SUCCESS && bits > 0) {
      t.memBusWidthBits = bits;
    }
  }

  if (t.memBusWidthBits > 0) {
    // Prefer transfer rate if NVML provides it.
    // BW(GB/s) = (transferRate(MT/s) * busWidth(bits)) / 8 / 1000.
    if (t.memTransferRateMHz > 0) {
      t.maxMemBandwidthGBps = (static_cast<double>(t.memTransferRateMHz) * (static_cast<double>(t.memBusWidthBits) / 8.0)) / 1000.0;
    } else if (t.memClockMHz > 0) {
      // DDR-style estimate: BW(GB/s) = memClock(MHz) * (busWidth/8 bytes) * 2 / 1000.
      t.maxMemBandwidthGBps = (static_cast<double>(t.memClockMHz) * (static_cast<double>(t.memBusWidthBits) / 8.0) * 2.0) / 1000.0;
    }
  }

  return t;
}

// Forward declarations for helpers defined later in this file.
static std::optional<NvmlPcieThroughput> readNvmlPcieThroughputWithSession(nvmlDevice_t dev, NvmlApi& a);
static std::optional<NvmlPcieLink> readNvmlPcieLinkWithSession(nvmlDevice_t dev, NvmlApi& a);

// ---- Unsafe (in-process) implementations ----
static std::optional<unsigned int> nvmlGpuCountUnsafe() {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  return count;
}

static std::optional<NvmlTelemetry> readNvmlTelemetryForGpuUnsafe(unsigned int index) {
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

static std::optional<NvmlTelemetry> readNvmlTelemetryUnsafe() {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0) return std::nullopt;

  NvmlTelemetry agg;
  double maxUtil = 0.0;
  double maxMemUtil = 0.0;
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
    maxMemUtil = std::max(maxMemUtil, t->memUtilPct);
    sumUsed += t->memUsedGiB;
    sumTotal += t->memTotalGiB;
    sumPower += t->powerWatts;
    maxTemp = std::max(maxTemp, t->tempC);

    if (const auto p = parsePstateNumber(t->pstate)) {
      if (!bestP || *p < *bestP) bestP = *p;
    }
  }

  if (sumTotal <= 0.0 && sumPower <= 0.0 && maxUtil <= 0.0) return std::nullopt;

  agg.gpuUtilPct = maxUtil;
  agg.memUtilPct = maxMemUtil;
  agg.memUsedGiB = sumUsed;
  agg.memTotalGiB = sumTotal;
  agg.powerWatts = sumPower;
  agg.tempC = maxTemp;
  agg.pstate = bestP ? ("P" + std::to_string(*bestP)) : std::string{};
  return agg;
}

static std::optional<NvmlPcieThroughput> readNvmlPcieThroughputForGpuUnsafe(unsigned int index) {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0 || index >= count) return std::nullopt;

  nvmlDevice_t dev{};
  if (a.nvmlDeviceGetHandleByIndex_v2(index, &dev) != NVML_SUCCESS) return std::nullopt;
  return readNvmlPcieThroughputWithSession(dev, a);
}

static std::optional<NvmlPcieLink> readNvmlPcieLinkForGpuUnsafe(unsigned int index) {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0 || index >= count) return std::nullopt;

  nvmlDevice_t dev{};
  if (a.nvmlDeviceGetHandleByIndex_v2(index, &dev) != NVML_SUCCESS) return std::nullopt;
  return readNvmlPcieLinkWithSession(dev, a);
}

static std::optional<NvmlPcieThroughput> readNvmlPcieThroughputUnsafe() {
  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  unsigned int count = 0;
  if (a.nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return std::nullopt;
  if (count == 0) return std::nullopt;

  NvmlPcieThroughput agg;
  bool any = false;
  for (unsigned int i = 0; i < count; ++i) {
    nvmlDevice_t dev{};
    if (a.nvmlDeviceGetHandleByIndex_v2(i, &dev) != NVML_SUCCESS) continue;
    const auto t = readNvmlPcieThroughputWithSession(dev, a);
    if (!t) continue;
    agg.rxMBps += t->rxMBps;
    agg.txMBps += t->txMBps;
    any = true;
  }
  if (!any) return std::nullopt;
  return agg;
}

static std::optional<std::string> readNvmlSystemStringUnsafe(nvmlReturn_t (*fn)(char*, unsigned int)) {
  if (!fn) return std::nullopt;

  NvmlApi& a = api();
  if (!a.ok()) return std::nullopt;

  NvmlSession sess;
  if (!sess.inited) return std::nullopt;

  std::array<char, 128> buf{};
  if (fn(buf.data(), static_cast<unsigned int>(buf.size())) != NVML_SUCCESS) return std::nullopt;
  buf.back() = '\0';

  const std::string s(buf.data());
  if (s.empty()) return std::nullopt;
  return s;
}

static std::optional<NvmlPcieThroughput> readNvmlPcieThroughputWithSession(nvmlDevice_t dev, NvmlApi& a) {
  if (!a.nvmlDeviceGetPcieThroughput) return std::nullopt;

  unsigned int rxKBps = 0;
  unsigned int txKBps = 0;

  if (a.nvmlDeviceGetPcieThroughput(dev, NVML_PCIE_UTIL_RX_BYTES, &rxKBps) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetPcieThroughput(dev, NVML_PCIE_UTIL_TX_BYTES, &txKBps) != NVML_SUCCESS) return std::nullopt;

  NvmlPcieThroughput t;
  t.rxMBps = static_cast<double>(rxKBps) / 1024.0;
  t.txMBps = static_cast<double>(txKBps) / 1024.0;
  return t;
}

static std::optional<NvmlPcieLink> readNvmlPcieLinkWithSession(nvmlDevice_t dev, NvmlApi& a) {
  if (!a.nvmlDeviceGetCurrPcieLinkGeneration || !a.nvmlDeviceGetCurrPcieLinkWidth) return std::nullopt;

  unsigned int gen = 0;
  unsigned int width = 0;

  if (a.nvmlDeviceGetCurrPcieLinkGeneration(dev, &gen) != NVML_SUCCESS) return std::nullopt;
  if (a.nvmlDeviceGetCurrPcieLinkWidth(dev, &width) != NVML_SUCCESS) return std::nullopt;

  if (gen == 0 || width == 0) return std::nullopt;

  NvmlPcieLink l;
  l.generation = gen;
  l.width = width;
  return l;
}

}  // namespace

std::optional<unsigned int> nvmlGpuCount() {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptU32Msg>(
      [&](OptU32Msg& out) {
        if (const auto r = nvmlGpuCountUnsafe()) {
          out.has = 1;
          out.v = static_cast<std::uint32_t>(*r);
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return static_cast<unsigned int>(msg->v);
#else
  return nvmlGpuCountUnsafe();
#endif
}

std::optional<unsigned int> nvmlGpuCountNoFork() {
  return nvmlGpuCountUnsafe();
}

std::optional<NvmlTelemetry> readNvmlTelemetryForGpu(unsigned int index) {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptTelemetryMsg>(
      [&](OptTelemetryMsg& out) {
        if (const auto r = readNvmlTelemetryForGpuUnsafe(index)) {
          out.has = 1;
          out.gpuUtilPct = r->gpuUtilPct;
          out.memUtilPct = r->memUtilPct;
          out.memUsedGiB = r->memUsedGiB;
          out.memTotalGiB = r->memTotalGiB;
          out.powerWatts = r->powerWatts;
          out.tempC = r->tempC;
          std::strncpy(out.pstate, r->pstate.c_str(), sizeof(out.pstate) - 1);
          out.pstate[sizeof(out.pstate) - 1] = '\0';

          out.gpuClockMHz = static_cast<std::uint32_t>(r->gpuClockMHz);
          out.memClockMHz = static_cast<std::uint32_t>(r->memClockMHz);
          out.memTransferRateMHz = static_cast<std::uint32_t>(r->memTransferRateMHz);
          out.smMajor = static_cast<std::uint32_t>(r->smMajor);
          out.smMinor = static_cast<std::uint32_t>(r->smMinor);

          out.maxPowerLimitWatts = r->maxPowerLimitWatts;
          out.memBusWidthBits = static_cast<std::uint32_t>(r->memBusWidthBits);
          out.maxMemBandwidthGBps = r->maxMemBandwidthGBps;
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;

  NvmlTelemetry t;
  t.gpuUtilPct = msg->gpuUtilPct;
  t.memUtilPct = msg->memUtilPct;
  t.memUsedGiB = msg->memUsedGiB;
  t.memTotalGiB = msg->memTotalGiB;
  t.powerWatts = msg->powerWatts;
  t.tempC = msg->tempC;
  t.pstate = std::string(msg->pstate);
  t.gpuClockMHz = static_cast<unsigned int>(msg->gpuClockMHz);
  t.memClockMHz = static_cast<unsigned int>(msg->memClockMHz);
  t.memTransferRateMHz = static_cast<unsigned int>(msg->memTransferRateMHz);
  t.smMajor = static_cast<unsigned int>(msg->smMajor);
  t.smMinor = static_cast<unsigned int>(msg->smMinor);
  t.maxPowerLimitWatts = msg->maxPowerLimitWatts;
  t.memBusWidthBits = static_cast<unsigned int>(msg->memBusWidthBits);
  t.maxMemBandwidthGBps = msg->maxMemBandwidthGBps;
  return t;
#else
  return readNvmlTelemetryForGpuUnsafe(index);
#endif
}

std::optional<NvmlTelemetry> readNvmlTelemetry() {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptTelemetryMsg>(
      [&](OptTelemetryMsg& out) {
        if (const auto r = readNvmlTelemetryUnsafe()) {
          out.has = 1;
          out.gpuUtilPct = r->gpuUtilPct;
          out.memUtilPct = r->memUtilPct;
          out.memUsedGiB = r->memUsedGiB;
          out.memTotalGiB = r->memTotalGiB;
          out.powerWatts = r->powerWatts;
          out.tempC = r->tempC;
          std::strncpy(out.pstate, r->pstate.c_str(), sizeof(out.pstate) - 1);
          out.pstate[sizeof(out.pstate) - 1] = '\0';

          out.gpuClockMHz = static_cast<std::uint32_t>(r->gpuClockMHz);
          out.memClockMHz = static_cast<std::uint32_t>(r->memClockMHz);
          out.memTransferRateMHz = static_cast<std::uint32_t>(r->memTransferRateMHz);
          out.smMajor = static_cast<std::uint32_t>(r->smMajor);
          out.smMinor = static_cast<std::uint32_t>(r->smMinor);

          out.maxPowerLimitWatts = r->maxPowerLimitWatts;
          out.memBusWidthBits = static_cast<std::uint32_t>(r->memBusWidthBits);
          out.maxMemBandwidthGBps = r->maxMemBandwidthGBps;
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;

  NvmlTelemetry t;
  t.gpuUtilPct = msg->gpuUtilPct;
  t.memUtilPct = msg->memUtilPct;
  t.memUsedGiB = msg->memUsedGiB;
  t.memTotalGiB = msg->memTotalGiB;
  t.powerWatts = msg->powerWatts;
  t.tempC = msg->tempC;
  t.pstate = std::string(msg->pstate);
  t.gpuClockMHz = static_cast<unsigned int>(msg->gpuClockMHz);
  t.memClockMHz = static_cast<unsigned int>(msg->memClockMHz);
  t.memTransferRateMHz = static_cast<unsigned int>(msg->memTransferRateMHz);
  t.smMajor = static_cast<unsigned int>(msg->smMajor);
  t.smMinor = static_cast<unsigned int>(msg->smMinor);
  t.maxPowerLimitWatts = msg->maxPowerLimitWatts;
  t.memBusWidthBits = static_cast<unsigned int>(msg->memBusWidthBits);
  t.maxMemBandwidthGBps = msg->maxMemBandwidthGBps;
  return t;
#else
  return readNvmlTelemetryUnsafe();
#endif
}

std::optional<NvmlPcieThroughput> readNvmlPcieThroughputForGpu(unsigned int index) {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptPcieThroughputMsg>(
      [&](OptPcieThroughputMsg& out) {
        if (const auto r = readNvmlPcieThroughputForGpuUnsafe(index)) {
          out.has = 1;
          out.t = *r;
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return msg->t;
#else
  return readNvmlPcieThroughputForGpuUnsafe(index);
#endif
}

std::optional<NvmlPcieLink> readNvmlPcieLinkForGpu(unsigned int index) {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptPcieLinkMsg>(
      [&](OptPcieLinkMsg& out) {
        if (const auto r = readNvmlPcieLinkForGpuUnsafe(index)) {
          out.has = 1;
          out.l = *r;
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return msg->l;
#else
  return readNvmlPcieLinkForGpuUnsafe(index);
#endif
}

std::optional<NvmlPcieThroughput> readNvmlPcieThroughput() {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptPcieThroughputMsg>(
      [&](OptPcieThroughputMsg& out) {
        if (const auto r = readNvmlPcieThroughputUnsafe()) {
          out.has = 1;
          out.t = *r;
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return msg->t;
#else
  return readNvmlPcieThroughputUnsafe();
#endif
}

std::optional<std::string> readNvmlGpuNameForGpu(unsigned int index) {
#if defined(__linux__)
  const auto msg = callWithTimeout<OptStringMsg>(
      [&](OptStringMsg& out) {
        if (const auto r = readNvmlGpuNameForGpuUnsafe(index)) {
          out.has = 1;
          const std::size_t n = std::min<std::size_t>(r->size(), sizeof(out.buf) - 1);
          std::memcpy(out.buf, r->data(), n);
          out.buf[n] = '\0';
          out.len = static_cast<std::uint16_t>(n);
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return std::string(msg->buf, msg->buf + msg->len);
#else
  return readNvmlGpuNameForGpuUnsafe(index);
#endif
}

std::optional<std::string> readNvmlGpuNameForGpuNoFork(unsigned int index) {
  return readNvmlGpuNameForGpuUnsafe(index);
}

std::optional<std::string> readNvmlLibraryVersion() {
#if defined(__linux__)
  NvmlApi& a = api();
  const auto msg = callWithTimeout<OptStringMsg>(
      [&](OptStringMsg& out) {
        if (const auto r = readNvmlSystemStringUnsafe(a.nvmlSystemGetNVMLVersion)) {
          out.has = 1;
          const std::size_t n = std::min<std::size_t>(r->size(), sizeof(out.buf) - 1);
          std::memcpy(out.buf, r->data(), n);
          out.buf[n] = '\0';
          out.len = static_cast<std::uint16_t>(n);
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return std::string(msg->buf, msg->buf + msg->len);
#else
  NvmlApi& a = api();
  return readNvmlSystemStringUnsafe(a.nvmlSystemGetNVMLVersion);
#endif
}

std::optional<std::string> readNvmlDriverVersion() {
#if defined(__linux__)
  NvmlApi& a = api();
  const auto msg = callWithTimeout<OptStringMsg>(
      [&](OptStringMsg& out) {
        if (const auto r = readNvmlSystemStringUnsafe(a.nvmlSystemGetDriverVersion)) {
          out.has = 1;
          const std::size_t n = std::min<std::size_t>(r->size(), sizeof(out.buf) - 1);
          std::memcpy(out.buf, r->data(), n);
          out.buf[n] = '\0';
          out.len = static_cast<std::uint16_t>(n);
        }
      },
      kNvmlCallTimeout);
  if (!msg || msg->has == 0) return std::nullopt;
  return std::string(msg->buf, msg->buf + msg->len);
#else
  NvmlApi& a = api();
  return readNvmlSystemStringUnsafe(a.nvmlSystemGetDriverVersion);
#endif
}

}  // namespace aiz
