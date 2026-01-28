#include <aiz/metrics/linux_gpu_sysfs.h>

#include <aiz/metrics/amd_rocm_smi.h>

#if defined(__linux__)
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace aiz {

namespace {

static std::optional<std::string> readTextFile(const fs::path& p) {
  std::ifstream f(p);
  if (!f) return std::nullopt;
  std::string s;
  std::getline(f, s);
  if (!f && s.empty()) return std::nullopt;
  // Trim trailing whitespace/newline.
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  // Trim leading spaces.
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  if (s.empty()) return std::nullopt;
  return s;
}

static std::optional<std::uint64_t> readU64(const fs::path& p) {
  const auto s = readTextFile(p);
  if (!s) return std::nullopt;
  try {
    std::size_t idx = 0;
    const std::uint64_t v = std::stoull(*s, &idx, 0);
    (void)idx;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

static std::optional<double> readDouble(const fs::path& p) {
  const auto s = readTextFile(p);
  if (!s) return std::nullopt;
  try {
    return std::stod(*s);
  } catch (...) {
    return std::nullopt;
  }
}

static std::optional<fs::path> firstExisting(const std::vector<fs::path>& paths) {
  for (const auto& p : paths) {
    std::error_code ec;
    if (fs::exists(p, ec) && !ec) return p;
  }
  return std::nullopt;
}

static GpuVendor vendorFromHex(const std::string& v) {
  // Expected forms: "0x10de", "0x1002", "0x8086".
  std::string s = v;
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "0x10de") return GpuVendor::Nvidia;
  if (s == "0x1002" || s == "0x1022") return GpuVendor::Amd;   // 0x1002 is AMD GPU; keep 0x1022 as lenient AMD.
  if (s == "0x8086") return GpuVendor::Intel;
  return GpuVendor::Unknown;
}

static std::optional<std::string> readDriverFromUevent(const fs::path& devPath) {
  std::ifstream f(devPath / "uevent");
  if (!f) return std::nullopt;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("DRIVER=", 0) == 0) {
      return line.substr(std::string("DRIVER=").size());
    }
  }
  return std::nullopt;
}

static std::optional<std::string> readPciSlotFromUevent(const fs::path& devPath) {
  std::ifstream f(devPath / "uevent");
  if (!f) return std::nullopt;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("PCI_SLOT_NAME=", 0) == 0) {
      return line.substr(std::string("PCI_SLOT_NAME=").size());
    }
  }
  return std::nullopt;
}

static bool isDisplayClass(const fs::path& devPath) {
  const auto cls = readTextFile(devPath / "class");
  if (!cls) return true;  // be permissive
  // PCI class: 0x03xxxx = display controller.
  return cls->rfind("0x03", 0) == 0;
}

static std::optional<fs::path> findHwmonDir(const fs::path& devPath) {
  const fs::path hw = devPath / "hwmon";
  std::error_code ec;
  if (!fs::exists(hw, ec) || ec) return std::nullopt;
  for (const auto& ent : fs::directory_iterator(hw, ec)) {
    if (ec) break;
    if (ent.is_directory()) return ent.path();
  }
  return std::nullopt;
}

static void fillTempPowerFromHwmon(const fs::path& devPath, LinuxGpuTelemetry& t) {
  const auto hw = findHwmonDir(devPath);
  if (!hw) return;

  // Temperature is typically millidegrees C.
  if (!t.tempC) {
    if (const auto tempMilli = readU64(*hw / "temp1_input")) {
      t.tempC = static_cast<double>(*tempMilli) / 1000.0;
    }
  }

  // Power is typically microwatts.
  if (!t.watts) {
    if (const auto pUw = readU64(*hw / "power1_average")) {
      t.watts = static_cast<double>(*pUw) / 1'000'000.0;
    } else if (const auto pUw2 = readU64(*hw / "power1_input")) {
      t.watts = static_cast<double>(*pUw2) / 1'000'000.0;
    }
  }
}

static void fillAmdTelemetry(const fs::path& devPath, LinuxGpuTelemetry& t) {
  t.source = "amdgpu-sysfs";

  if (const auto u = readDouble(devPath / "gpu_busy_percent")) {
    t.utilPct = *u;
  }

  // VRAM bytes.
  if (const auto used = readU64(devPath / "mem_info_vram_used")) {
    t.vramUsedGiB = static_cast<double>(*used) / (1024.0 * 1024.0 * 1024.0);
  }
  if (const auto total = readU64(devPath / "mem_info_vram_total")) {
    t.vramTotalGiB = static_cast<double>(*total) / (1024.0 * 1024.0 * 1024.0);
  }

  // A simple perf level hint if present.
  if (const auto ps = readTextFile(devPath / "power_dpm_force_performance_level")) {
    t.pstate = *ps;
  }

  fillTempPowerFromHwmon(devPath, t);
}

static void fillIntelTelemetry(const fs::path& devPath, const std::string& driver, LinuxGpuTelemetry& t) {
  t.source = (driver.empty() ? std::string("intel-sysfs") : (driver + "-sysfs"));

  // Utilization files vary across kernels/drivers; probe a small set.
  const auto utilPath = firstExisting({
      devPath / "gt_busy_percent",
      devPath / "gt" / "gt0" / "rps_busy_percent",
      devPath / "gt" / "gt0" / "busy_percent",
  });
  if (utilPath) {
    if (const auto u = readDouble(*utilPath)) {
      t.utilPct = *u;
    }
  }

  // dGPU VRAM exists on some Intel parts/drivers (xe); on iGPU, this often won't exist.
  if (const auto used = readU64(devPath / "mem_info_vram_used")) {
    t.vramUsedGiB = static_cast<double>(*used) / (1024.0 * 1024.0 * 1024.0);
  }
  if (const auto total = readU64(devPath / "mem_info_vram_total")) {
    t.vramTotalGiB = static_cast<double>(*total) / (1024.0 * 1024.0 * 1024.0);
  }

  fillTempPowerFromHwmon(devPath, t);
}

}  // namespace

std::vector<LinuxGpuDevice> enumerateLinuxGpus() {
  std::vector<LinuxGpuDevice> out;

  const fs::path drm("/sys/class/drm");
  std::error_code ec;
  if (!fs::exists(drm, ec) || ec) return out;

  // Collect cardN entries.
  std::vector<std::pair<unsigned int, fs::path>> cards;
  for (const auto& ent : fs::directory_iterator(drm, ec)) {
    if (ec) break;
    const auto name = ent.path().filename().string();
    if (name.rfind("card", 0) != 0) continue;
    if (name.size() <= 4) continue;
    // Parse trailing number.
    try {
      const unsigned int idx = static_cast<unsigned int>(std::stoul(name.substr(4)));
      cards.emplace_back(idx, ent.path());
    } catch (...) {
      continue;
    }
  }

  std::sort(cards.begin(), cards.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  unsigned int stable = 0;
  for (const auto& [cardIdx, cardPath] : cards) {
    (void)cardIdx;
    const fs::path devPath = cardPath / "device";
    if (!fs::exists(devPath, ec) || ec) continue;
    if (!isDisplayClass(devPath)) continue;

    LinuxGpuDevice d;
    d.index = stable++;
    d.drmCard = cardPath.filename().string();
    d.sysfsDevicePath = devPath.string();
    d.pciSlotName = readPciSlotFromUevent(devPath).value_or(std::string{});

    if (const auto vend = readTextFile(devPath / "vendor")) {
      d.vendor = vendorFromHex(*vend);
    }

    d.driver = readDriverFromUevent(devPath).value_or(std::string{});

    out.push_back(std::move(d));
  }

  return out;
}

unsigned int linuxGpuCount() {
  return static_cast<unsigned int>(enumerateLinuxGpus().size());
}

std::optional<LinuxGpuTelemetry> readLinuxGpuTelemetry(unsigned int index) {
  const auto gpus = enumerateLinuxGpus();
  if (index >= gpus.size()) return std::nullopt;

  const fs::path devPath(gpus[index].sysfsDevicePath);
  LinuxGpuTelemetry t;

  // Vendor-specific probes (best-effort).
  if (gpus[index].vendor == GpuVendor::Amd || gpus[index].driver == "amdgpu") {
    // Prefer ROCm SMI, then fall back to sysfs.
    if (const auto rocm = readRocmSmiTelemetryForPciBusId(gpus[index].pciSlotName)) {
      return rocm;
    }

    // Fallback: use ROCm SMI by AMD-index if PCI mapping isn't available.
    unsigned int amdIndex = 0;
    for (unsigned int i = 0; i < gpus.size(); ++i) {
      if (gpus[i].vendor == GpuVendor::Amd || gpus[i].driver == "amdgpu") {
        if (i == index) break;
        ++amdIndex;
      }
    }
    if (const auto rocm = readRocmSmiTelemetryForIndex(amdIndex)) {
      return rocm;
    }

    fillAmdTelemetry(devPath, t);
  } else if (gpus[index].vendor == GpuVendor::Intel || gpus[index].driver == "i915" || gpus[index].driver == "xe") {
    fillIntelTelemetry(devPath, gpus[index].driver, t);
  } else {
    // Unknown vendor: still try generic temp/power from hwmon.
    t.source = "sysfs";
    fillTempPowerFromHwmon(devPath, t);
  }

  const bool any = t.utilPct || t.vramUsedGiB || t.vramTotalGiB || t.watts || t.tempC || !t.pstate.empty();
  if (!any) return std::nullopt;
  return t;
}

}  // namespace aiz

#else

namespace aiz {

std::vector<LinuxGpuDevice> enumerateLinuxGpus() { return {}; }
unsigned int linuxGpuCount() { return 0; }
std::optional<LinuxGpuTelemetry> readLinuxGpuTelemetry(unsigned int) { return std::nullopt; }

}  // namespace aiz

#endif
