#include <aiz/metrics/amd_rocm_smi.h>

#include <aiz/platform/dynlib.h>

#if defined(AI_Z_PLATFORM_LINUX)
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace aiz {
namespace {

constexpr std::chrono::milliseconds kRocmSmiCallTimeout{700};

#if defined(AI_Z_PLATFORM_LINUX)
struct OptU32Msg {
  std::uint8_t has = 0;
  std::uint32_t v = 0;
};

struct OptTelemetryMsg {
  std::uint8_t ok = 0;
  std::uint8_t hasUtil = 0;
  std::uint8_t hasVramUsed = 0;
  std::uint8_t hasVramTotal = 0;
  std::uint8_t hasWatts = 0;
  std::uint8_t hasTemp = 0;
  std::uint8_t hasPstate = 0;

  double utilPct = 0.0;
  double vramUsedGiB = 0.0;
  double vramTotalGiB = 0.0;
  double watts = 0.0;
  double tempC = 0.0;
  char pstate[16]{};
};

struct OptBdfMsg {
  std::uint8_t has = 0;
  char bdf[32]{};
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
    ::close(fds[0]);
    MsgT msg{};
    fn(msg);
    (void)writeExact(fds[1], &msg, sizeof(MsgT));
    ::close(fds[1]);
    _exit(0);
  }

  ::close(fds[1]);

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fds[0], &rfds);

  struct timeval tv;
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

  const int sel = ::select(fds[0] + 1, &rfds, nullptr, nullptr, &tv);
  if (sel <= 0) {
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
#else

template <typename MsgT, typename Fn>
static std::optional<MsgT> callWithTimeout(Fn&& fn, std::chrono::milliseconds) {
  MsgT msg{};
  fn(msg);
  return msg;
}
#endif

using rsmi_status_t = int;
constexpr rsmi_status_t RSMI_STATUS_SUCCESS = 0;

using rsmi_init_t = rsmi_status_t (*)(std::uint64_t);
using rsmi_shut_down_t = rsmi_status_t (*)();
using rsmi_num_monitor_devices_t = rsmi_status_t (*)(std::uint32_t*);
using rsmi_dev_busy_percent_get_t = rsmi_status_t (*)(std::uint32_t, std::uint32_t*);
using rsmi_dev_memory_usage_get_t = rsmi_status_t (*)(std::uint32_t, int, std::uint64_t*);
using rsmi_dev_memory_total_get_t = rsmi_status_t (*)(std::uint32_t, int, std::uint64_t*);
using rsmi_dev_power_ave_get_t = rsmi_status_t (*)(std::uint32_t, std::uint32_t, std::uint64_t*);
using rsmi_dev_temp_metric_get_t = rsmi_status_t (*)(std::uint32_t, std::uint32_t, int, std::int64_t*);
using rsmi_dev_perf_level_get_t = rsmi_status_t (*)(std::uint32_t, int*);
using rsmi_dev_pci_id_get_t = rsmi_status_t (*)(std::uint32_t, std::uint64_t*);

constexpr int RSMI_MEM_TYPE_VRAM = 0;
constexpr int RSMI_TEMP_CURRENT = 0;

struct RocmSmiApi {
  std::unique_ptr<platform::DynamicLibrary> lib;
  bool ok = false;

  rsmi_init_t rsmi_init = nullptr;
  rsmi_shut_down_t rsmi_shut_down = nullptr;
  rsmi_num_monitor_devices_t rsmi_num_monitor_devices = nullptr;
  rsmi_dev_busy_percent_get_t rsmi_dev_busy_percent_get = nullptr;
  rsmi_dev_memory_usage_get_t rsmi_dev_memory_usage_get = nullptr;
  rsmi_dev_memory_total_get_t rsmi_dev_memory_total_get = nullptr;
  rsmi_dev_power_ave_get_t rsmi_dev_power_ave_get = nullptr;
  rsmi_dev_temp_metric_get_t rsmi_dev_temp_metric_get = nullptr;
  rsmi_dev_perf_level_get_t rsmi_dev_perf_level_get = nullptr;
  rsmi_dev_pci_id_get_t rsmi_dev_pci_id_get = nullptr;
};

static RocmSmiApi* rocmApi(std::string* errOut = nullptr) {
  static RocmSmiApi api;
  static bool initialized = false;
  if (initialized) return api.ok ? &api : nullptr;
  initialized = true;

  std::string err;
  const std::vector<const char*> candidates = {
      "librocm_smi64.so.1",
      "librocm_smi64.so",
      "librocm_smi64.so.6",
  };

  api.lib = platform::loadLibrary(candidates, &err);
  if (!api.lib || !api.lib->isValid()) {
    if (errOut) *errOut = err;
    api.ok = false;
    return nullptr;
  }

  if (!api.lib->loadRequired("rsmi_init", api.rsmi_init, err) ||
      !api.lib->loadRequired("rsmi_shut_down", api.rsmi_shut_down, err) ||
      !api.lib->loadRequired("rsmi_num_monitor_devices", api.rsmi_num_monitor_devices, err) ||
      !api.lib->loadRequired("rsmi_dev_busy_percent_get", api.rsmi_dev_busy_percent_get, err) ||
      !api.lib->loadRequired("rsmi_dev_memory_usage_get", api.rsmi_dev_memory_usage_get, err) ||
      !api.lib->loadRequired("rsmi_dev_memory_total_get", api.rsmi_dev_memory_total_get, err)) {
    if (errOut) *errOut = err;
    api.ok = false;
    return nullptr;
  }

  api.lib->loadSymbol("rsmi_dev_power_ave_get", api.rsmi_dev_power_ave_get);
  api.lib->loadSymbol("rsmi_dev_temp_metric_get", api.rsmi_dev_temp_metric_get);
  api.lib->loadSymbol("rsmi_dev_perf_level_get", api.rsmi_dev_perf_level_get);
  api.lib->loadSymbol("rsmi_dev_pci_id_get", api.rsmi_dev_pci_id_get);

  api.ok = true;
  return &api;
}

static std::string perfLevelToString(int perf) {
  switch (perf) {
    case 0:
      return "auto";
    case 1:
      return "low";
    case 2:
      return "high";
    case 3:
      return "manual";
    case 4:
      return "stable";
    default:
      return "";
  }
}

static std::optional<std::string> bdfFromRocmPciId(std::uint64_t id) {
  // Best-effort decode: domain:bus:device.function
  // This follows the common PCI BDF packing used by some ROCm SMI versions.
  const std::uint16_t domain = static_cast<std::uint16_t>((id >> 32) & 0xffff);
  const std::uint8_t bus = static_cast<std::uint8_t>((id >> 8) & 0xff);
  const std::uint8_t device = static_cast<std::uint8_t>((id >> 3) & 0x1f);
  const std::uint8_t function = static_cast<std::uint8_t>(id & 0x7);

  if (bus == 0 && device == 0 && function == 0 && domain == 0) return std::nullopt;

  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "%04x:%02x:%02x.%u", domain, bus, device, function);
  return std::string(buf);
}

static std::optional<LinuxGpuTelemetry> readTelemetryForIndex(unsigned int index) {
  const auto msg = callWithTimeout<OptTelemetryMsg>([&](OptTelemetryMsg& m) {
    std::string err;
    auto* api = rocmApi(&err);
    if (!api) return;

    if (api->rsmi_init(0) != RSMI_STATUS_SUCCESS) return;

    std::uint32_t count = 0;
    if (api->rsmi_num_monitor_devices(&count) != RSMI_STATUS_SUCCESS || index >= count) {
      api->rsmi_shut_down();
      return;
    }

    m.ok = 1;

    std::uint32_t util = 0;
    if (api->rsmi_dev_busy_percent_get(index, &util) == RSMI_STATUS_SUCCESS) {
      m.hasUtil = 1;
      m.utilPct = static_cast<double>(util);
    }

    std::uint64_t used = 0;
    if (api->rsmi_dev_memory_usage_get(index, RSMI_MEM_TYPE_VRAM, &used) == RSMI_STATUS_SUCCESS) {
      m.hasVramUsed = 1;
      m.vramUsedGiB = static_cast<double>(used) / (1024.0 * 1024.0 * 1024.0);
    }

    std::uint64_t total = 0;
    if (api->rsmi_dev_memory_total_get(index, RSMI_MEM_TYPE_VRAM, &total) == RSMI_STATUS_SUCCESS) {
      m.hasVramTotal = 1;
      m.vramTotalGiB = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);
    }

    if (api->rsmi_dev_power_ave_get) {
      std::uint64_t powerUw = 0;
      if (api->rsmi_dev_power_ave_get(index, 0, &powerUw) == RSMI_STATUS_SUCCESS) {
        m.hasWatts = 1;
        m.watts = static_cast<double>(powerUw) / 1'000'000.0;
      }
    }

    if (api->rsmi_dev_temp_metric_get) {
      std::int64_t tempMilli = 0;
      if (api->rsmi_dev_temp_metric_get(index, 0, RSMI_TEMP_CURRENT, &tempMilli) == RSMI_STATUS_SUCCESS) {
        m.hasTemp = 1;
        m.tempC = static_cast<double>(tempMilli) / 1000.0;
      }
    }

    if (api->rsmi_dev_perf_level_get) {
      int perf = 0;
      if (api->rsmi_dev_perf_level_get(index, &perf) == RSMI_STATUS_SUCCESS) {
        const std::string p = perfLevelToString(perf);
        if (!p.empty()) {
          m.hasPstate = 1;
          std::snprintf(m.pstate, sizeof(m.pstate), "%s", p.c_str());
        }
      }
    }

    api->rsmi_shut_down();
  }, kRocmSmiCallTimeout);

  if (!msg || !msg->ok) return std::nullopt;

  LinuxGpuTelemetry t;
  t.source = "rocm-smi";
  if (msg->hasUtil) t.utilPct = msg->utilPct;
  if (msg->hasVramUsed) t.vramUsedGiB = msg->vramUsedGiB;
  if (msg->hasVramTotal) t.vramTotalGiB = msg->vramTotalGiB;
  if (msg->hasWatts) t.watts = msg->watts;
  if (msg->hasTemp) t.tempC = msg->tempC;
  if (msg->hasPstate) t.pstate = std::string(msg->pstate);

  const bool any = t.utilPct || t.vramUsedGiB || t.vramTotalGiB || t.watts || t.tempC || !t.pstate.empty();
  if (!any) return std::nullopt;
  return t;
}

static std::optional<std::string> bdfForIndex(unsigned int index) {
  const auto msg = callWithTimeout<OptBdfMsg>([&](OptBdfMsg& m) {
    std::string err;
    auto* api = rocmApi(&err);
    if (!api || !api->rsmi_dev_pci_id_get) return;

    if (api->rsmi_init(0) != RSMI_STATUS_SUCCESS) return;

    std::uint32_t count = 0;
    if (api->rsmi_num_monitor_devices(&count) != RSMI_STATUS_SUCCESS || index >= count) {
      api->rsmi_shut_down();
      return;
    }

    std::uint64_t id = 0;
    if (api->rsmi_dev_pci_id_get(index, &id) != RSMI_STATUS_SUCCESS) {
      api->rsmi_shut_down();
      return;
    }

    api->rsmi_shut_down();
    if (const auto bdf = bdfFromRocmPciId(id)) {
      m.has = 1;
      std::snprintf(m.bdf, sizeof(m.bdf), "%s", bdf->c_str());
    }
  }, kRocmSmiCallTimeout);

  if (!msg || !msg->has) return std::nullopt;
  return std::string(msg->bdf);
}

}  // namespace

std::optional<unsigned int> rocmSmiGpuCount() {
#if defined(AI_Z_PLATFORM_LINUX)
  const auto msg = callWithTimeout<OptU32Msg>([](OptU32Msg& m) {
    std::string err;
    auto* api = rocmApi(&err);
    if (!api) return;
    if (api->rsmi_init(0) != RSMI_STATUS_SUCCESS) return;

    std::uint32_t count = 0;
    if (api->rsmi_num_monitor_devices(&count) == RSMI_STATUS_SUCCESS) {
      m.has = 1;
      m.v = count;
    }
    api->rsmi_shut_down();
  }, kRocmSmiCallTimeout);

  if (!msg || !msg->has) return std::nullopt;
  return static_cast<unsigned int>(msg->v);
#else
  return std::nullopt;
#endif
}

std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForIndex(unsigned int index) {
#if defined(AI_Z_PLATFORM_LINUX)
  return readTelemetryForIndex(index);
#else
  (void)index;
  return std::nullopt;
#endif
}

std::optional<LinuxGpuTelemetry> readRocmSmiTelemetryForPciBusId(const std::string& pciBusId) {
#if defined(AI_Z_PLATFORM_LINUX)
  if (pciBusId.empty()) return std::nullopt;
  const auto count = rocmSmiGpuCount();
  if (!count || *count == 0) return std::nullopt;

  for (unsigned int i = 0; i < *count; ++i) {
    const auto bdf = bdfForIndex(i);
    if (!bdf) continue;
    if (*bdf == pciBusId) {
      return readTelemetryForIndex(i);
    }
  }
  return std::nullopt;
#else
  (void)pciBusId;
  return std::nullopt;
#endif
}

}  // namespace aiz
