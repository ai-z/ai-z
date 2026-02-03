#include <aiz/hw/hardware_info.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <string>
#include <thread>

#include <dlfcn.h>

#include <aiz/dyn/cuda.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/npu_info.h>

namespace aiz {

static constexpr const char* kUnknown = "--";

static std::string trim(std::string s) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static std::string normalizeOpenCLPlatformVersion(std::string s) {
  s = trim(std::move(s));
  if (s.rfind("OpenCL ", 0) == 0) s = trim(s.substr(std::string("OpenCL ").size()));
  if (s.empty()) return std::string(kUnknown);

  std::istringstream iss(s);
  std::vector<std::string> parts;
  std::string tok;
  while (iss >> tok) parts.push_back(tok);

  if (parts.empty()) return std::string(kUnknown);
  if (parts.size() == 1) return parts[0];

  std::ostringstream oss;
  oss << parts[0] << " (";
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (i != 1) oss << ' ';
    oss << parts[i];
  }
  oss << ')';
  return oss.str();
}

static std::optional<std::string> readFirstMatch(const char* path, const std::string& prefix) {
  std::ifstream in(path);
  if (!in.is_open()) return std::nullopt;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return trim(line.substr(prefix.size()));
    }
  }
  return std::nullopt;
}

static std::optional<std::string> readOsPrettyName() {
  std::ifstream in("/etc/os-release");
  if (!in.is_open()) return std::nullopt;

  std::string name;
  std::string version;
  std::string pretty;

  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (!val.empty() && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }

    if (key == "PRETTY_NAME") pretty = val;
    else if (key == "NAME") name = val;
    else if (key == "VERSION") version = val;
  }

  if (!pretty.empty()) return pretty;
  if (!name.empty() && !version.empty()) return name + " " + version;
  if (!name.empty()) return name;
  return std::nullopt;
}

static std::optional<std::string> runCommand(const std::string& cmd) {
  std::array<char, 4096> buf{};
  std::string out;

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return std::nullopt;

  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
    if (out.size() > 64 * 1024) break;
  }

  const int rc = pclose(pipe);
  if (rc != 0) return std::nullopt;

  out = trim(out);
  if (out.empty()) return std::nullopt;
  return out;
}

static std::optional<unsigned int> cudaCoresPerSm(unsigned int major, unsigned int minor) {
  // Best-effort mapping from CUDA compute capability to FP32 "CUDA cores" per SM.
  // This is not an NVML-provided attribute; it is derived from NVIDIA architecture.
  // If an unknown/unsupported architecture is encountered, return nullopt.
  switch (major) {
    case 2:  // Fermi
      return (minor == 1) ? 48u : 32u;
    case 3:  // Kepler
      return 192u;
    case 5:  // Maxwell
      return 128u;
    case 6:  // Pascal
      return (minor == 0) ? 64u : 128u;
    case 7:  // Volta/Turing
      return 64u;
    case 8:  // Ampere/Ada
      if (minor == 0) return 64u;  // A100-class
      if (minor == 6 || minor == 7 || minor == 9) return 128u;  // GA10x/Orin/Ada
      return std::nullopt;
    case 9:  // Hopper
      return 128u;
    default:
      return std::nullopt;
  }
}

static std::vector<std::string> probePerGpuLinesNvidia() {
  const auto n = nvmlGpuCount();
  if (!n || *n == 0) return {};

  auto fmtGFromGiB = [](double gib) -> std::string {
    const long rounded = static_cast<long>(std::llround(gib));
    if (std::abs(gib - static_cast<double>(rounded)) < 0.05) {
      return std::to_string(rounded) + "G";
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << gib << "G";
    return oss.str();
  };

  // Match the requested formatting:
  // GPU0: <name>
  //  Memory: 10G
  //  Max Graphics Clock: 2100 MHz
  //  Max Memory Clock: 10501 MHz
  //  Max Memory Transfer Rate: 21000 MT/s
  //  Max Memory Data Rate: 21.0 Gb/s
  //  Max Power Limit: 450 W
  //  Max Memory Bandwidth (est.): 1008.0 GB/s
  //  SM: 8.6
  const std::string indent = " ";
  std::vector<std::string> lines;
  lines.reserve(static_cast<std::size_t>(*n) * 3);

  for (unsigned int gpuIndex = 0; gpuIndex < *n; ++gpuIndex) {
    const std::string name = readNvmlGpuNameForGpu(gpuIndex).value_or(std::string(kUnknown));

    {
      std::ostringstream l;
      l << "GPU" << gpuIndex << ": " << name;
      lines.push_back(l.str());
    }

    if (const auto t = readNvmlTelemetryForGpu(gpuIndex)) {
      if (t->memTotalGiB > 0.0) {
        std::ostringstream l;
        l << indent << "Memory: " << fmtGFromGiB(t->memTotalGiB);
        lines.push_back(l.str());
      }

      {
        std::string cudaCoresStr = "not available";
        if (t->multiprocessorCount > 0 && t->smMajor > 0) {
          if (const auto cpsm = cudaCoresPerSm(t->smMajor, t->smMinor)) {
            const unsigned long long cores =
                static_cast<unsigned long long>(t->multiprocessorCount) * static_cast<unsigned long long>(*cpsm);
            if (cores > 0) cudaCoresStr = std::to_string(cores);
          }
        }
        std::ostringstream l;
        l << indent << "CUDA Cores: " << cudaCoresStr;
        lines.push_back(l.str());
      }

      if (t->gpuClockMHz > 0) {
        std::ostringstream l;
        l << indent << "Max Graphics Clock: " << t->gpuClockMHz << " MHz";
        lines.push_back(l.str());
      }

      if (t->memClockMHz > 0) {
        std::ostringstream l;
        l << indent << "Max Memory Clock: " << t->memClockMHz << " MHz";
        lines.push_back(l.str());
      }

      if (t->memTransferRateMHz > 0) {
        std::ostringstream l;
        l << indent << "Max Memory Transfer Rate: " << t->memTransferRateMHz << " MT/s";
        lines.push_back(l.str());

        {
          std::ostringstream lr;
          lr.setf(std::ios::fixed);
          lr.precision(1);
          lr << indent << "Max Memory Data Rate: " << (static_cast<double>(t->memTransferRateMHz) / 1000.0) << " Gb/s";
          lines.push_back(lr.str());
        }
      } else if (t->memClockMHz > 0) {
        // Fallback: estimate effective data rate as 2x memory clock.
        std::ostringstream lr;
        lr.setf(std::ios::fixed);
        lr.precision(1);
        lr << indent << "Max Memory Data Rate (est.): " << (static_cast<double>(t->memClockMHz) * 2.0 / 1000.0) << " Gb/s";
        lines.push_back(lr.str());
      }

      if (t->maxPowerLimitWatts > 0.0) {
        std::ostringstream l;
        l.setf(std::ios::fixed);
        l.precision(0);
        l << indent << "Max Power Limit: " << t->maxPowerLimitWatts << " W";
        lines.push_back(l.str());
      }

      if (t->maxMemBandwidthGBps > 0.0) {
        std::ostringstream l;
        l.setf(std::ios::fixed);
        l.precision(1);
        l << indent << "Max Memory Bandwidth (est.): " << t->maxMemBandwidthGBps << " GB/s";
        lines.push_back(l.str());
      }

      if (t->smMajor > 0) {
        std::ostringstream l;
        l << indent << "SM: " << t->smMajor << "." << t->smMinor;
        lines.push_back(l.str());
      }
    } else {
      std::ostringstream l;
      l << indent << "CUDA Cores: not available";
      lines.push_back(l.str());
    }

    if (const auto link = readNvmlPcieLinkForGpu(gpuIndex)) {
      std::ostringstream l;
      l.setf(std::ios::fixed);
      l.precision(1);
      l << indent << "PCIe: " << link->width << "x@" << static_cast<double>(link->generation);
      lines.push_back(l.str());
    }
  }

  return lines;
}

static std::string probeKernelVersion() {
  return runCommand("uname -r 2>/dev/null").value_or(kUnknown);
}

static std::string probeCudaVersion() {
  std::string err;
  const auto* cu = dyn::cuda::api(&err);
  if (!cu) return kUnknown;
  if (!cu->cuInit || !cu->cuDriverGetVersion) return kUnknown;
  if (cu->cuInit(0) != dyn::cuda::CUDA_SUCCESS) return kUnknown;

  int v = 0;
  if (cu->cuDriverGetVersion(&v) != dyn::cuda::CUDA_SUCCESS || v <= 0) return kUnknown;
  const int major = v / 1000;
  const int minor = (v % 1000) / 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

static std::string probeNvmlVersion() {
  if (auto v = readNvmlLibraryVersion()) return *v;
  return kUnknown;
}

static std::string probeVramSummary() {
  // NVIDIA: use NVML (no external tools).
  if (const auto t = readNvmlTelemetry()) {
    const auto n = nvmlGpuCount();
    const double total = t->memTotalGiB;
    if (total > 0.0) {
      std::ostringstream oss;
      oss.setf(std::ios::fixed);
      oss.precision(1);
      oss << total << " GiB";
      if (n && *n > 1) oss << " (" << *n << " GPUs)";
      return oss.str();
    }
  }

  return kUnknown;
}

static std::string probeRocmVersion() {
  // Common location on Linux.
  {
    std::ifstream in("/opt/rocm/.info/version");
    if (in.is_open()) {
      std::string v;
      std::getline(in, v);
      v = trim(v);
      if (!v.empty()) return v;
    }
  }

  // Best-effort CLI fallbacks.
  if (auto v = runCommand("rocm-smi --version 2>/dev/null | head -n 1")) return *v;
  if (auto v = runCommand("rocminfo 2>/dev/null | head -n 1")) return *v;
  return kUnknown;
}

static std::string probeOpenCLVersion() {
  // Prefer OpenCL ICD loader if present (no external tools needed).
  // We avoid including OpenCL headers; use minimal ABI.
  using cl_int = int;
  using cl_uint = unsigned int;
  using cl_platform_id = void*;
  constexpr cl_int CL_SUCCESS = 0;
  constexpr cl_uint CL_PLATFORM_VERSION = 0x0901;

  using clGetPlatformIDs_t = cl_int (*)(cl_uint, cl_platform_id*, cl_uint*);
  using clGetPlatformInfo_t = cl_int (*)(cl_platform_id, cl_uint, std::size_t, void*, std::size_t*);

  void* lib = dlopen("libOpenCL.so.1", RTLD_LAZY);
  if (!lib) lib = dlopen("libOpenCL.so", RTLD_LAZY);
  if (lib) {
    auto clGetPlatformIDs = reinterpret_cast<clGetPlatformIDs_t>(dlsym(lib, "clGetPlatformIDs"));
    auto clGetPlatformInfo = reinterpret_cast<clGetPlatformInfo_t>(dlsym(lib, "clGetPlatformInfo"));
    if (clGetPlatformIDs && clGetPlatformInfo) {
      cl_uint n = 0;
      if (clGetPlatformIDs(0, nullptr, &n) == CL_SUCCESS && n > 0) {
        std::vector<cl_platform_id> ids;
        ids.resize(n);
        if (clGetPlatformIDs(n, ids.data(), nullptr) == CL_SUCCESS) {
          std::unordered_set<std::string> versions;
          for (cl_uint i = 0; i < n; ++i) {
            std::size_t size = 0;
            if (!ids[i]) continue;
            if (clGetPlatformInfo(ids[i], CL_PLATFORM_VERSION, 0, nullptr, &size) != CL_SUCCESS || size == 0) continue;
            std::string s;
            s.resize(size);
            if (clGetPlatformInfo(ids[i], CL_PLATFORM_VERSION, size, s.data(), nullptr) != CL_SUCCESS) continue;
            const auto nul = s.find('\0');
            if (nul != std::string::npos) s.resize(nul);
            s = trim(s);
            if (s.rfind("OpenCL ", 0) == 0) s = trim(s.substr(std::string("OpenCL ").size()));
            if (!s.empty()) versions.insert(s);
          }
          dlclose(lib);
          if (!versions.empty()) {
            // Stable output: join in lexical order.
            std::vector<std::string> v;
            v.reserve(versions.size());
            for (const auto& s : versions) v.push_back(s);
            std::sort(v.begin(), v.end());
            std::ostringstream oss;
            for (std::size_t j = 0; j < v.size(); ++j) {
              if (j) oss << " | ";
              oss << normalizeOpenCLPlatformVersion(v[j]);
            }
            return oss.str();
          }
          return kUnknown;
        }
      }
    }
    dlclose(lib);
  }

  // Fallback: clinfo (if installed).
  if (auto v = runCommand(
          "clinfo -raw 2>/dev/null | awk -F: '/^Platform Version:/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return normalizeOpenCLPlatformVersion(*v);
  }
  if (auto v = runCommand(
          "clinfo 2>/dev/null | awk -F: '/Platform Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return normalizeOpenCLPlatformVersion(*v);
  }
  return kUnknown;
}

static std::string fmtVulkanVersion(std::uint32_t v) {
  const std::uint32_t major = (v >> 22) & 0x3ff;
  const std::uint32_t minor = (v >> 12) & 0x3ff;
  const std::uint32_t patch = v & 0xfff;
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

static std::string probeVulkanVersion() {
  // Prefer Vulkan loader API if present (no external tools needed).
  // vkEnumerateInstanceVersion is available since Vulkan 1.1.
  using VkResult = int;
  constexpr VkResult VK_SUCCESS = 0;
  using PFN_vkEnumerateInstanceVersion = VkResult (*)(std::uint32_t*);

  void* lib = dlopen("libvulkan.so.1", RTLD_LAZY);
  if (!lib) lib = dlopen("libvulkan.so", RTLD_LAZY);
  if (lib) {
    auto fn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(dlsym(lib, "vkEnumerateInstanceVersion"));
    if (fn) {
      std::uint32_t v = 0;
      if (fn(&v) == VK_SUCCESS && v != 0) {
        dlclose(lib);
        return fmtVulkanVersion(v);
      }
    }
    dlclose(lib);
  }

  // Fallback: vulkaninfo from vulkan-tools (if installed).
  if (auto v = runCommand(
          "vulkaninfo --summary 2>/dev/null | awk -F: '/Vulkan Instance Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return *v;
  }
  if (auto v = runCommand(
          "vulkaninfo 2>/dev/null | awk -F: '/Vulkan Instance Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return *v;
  }
  return kUnknown;
}

static std::string probeCpuName() {
  auto v = readFirstMatch("/proc/cpuinfo", "model name\t: ");
  if (v) return *v;
  v = readFirstMatch("/proc/cpuinfo", "Hardware\t: ");
  if (v) return *v;
  return kUnknown;
}

static std::optional<std::string> readTextFileTrim(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in.is_open()) return std::nullopt;
  std::string s;
  std::getline(in, s);
  s = trim(s);
  if (s.empty()) return std::nullopt;
  return s;
}

static std::optional<std::string> readUeventValue(const std::filesystem::path& ueventPath, const std::string& key) {
  std::ifstream in(ueventPath);
  if (!in.is_open()) return std::nullopt;
  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return trim(line.substr(prefix.size()));
    }
  }
  return std::nullopt;
}

static std::optional<std::string> symlinkBasename(const std::filesystem::path& p) {
  std::error_code ec;
  const auto link = std::filesystem::read_symlink(p, ec);
  if (ec) return std::nullopt;
  const auto name = link.filename().string();
  if (name.empty()) return std::nullopt;
  return name;
}

static std::string probeCpuLogicalCores() {
  std::ifstream in("/proc/cpuinfo");
  if (in.is_open()) {
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
      if (line.rfind("processor", 0) == 0) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) ++count;
      }
    }
    if (count > 0) return std::to_string(count);
  }

  const unsigned int hc = std::thread::hardware_concurrency();
  if (hc > 0) return std::to_string(hc);
  return kUnknown;
}

static std::string probeCpuPhysicalCores() {
  // Best-effort: count unique (physical id, core id) pairs.
  std::ifstream in("/proc/cpuinfo");
  if (!in.is_open()) return kUnknown;

  std::unordered_set<std::string> unique;
  unique.reserve(128);

  std::string line;
  std::string phys;
  std::string core;

  auto flush = [&]() {
    if (!phys.empty() && !core.empty()) {
      unique.insert(phys + ":" + core);
    }
    phys.clear();
    core.clear();
  };

  while (std::getline(in, line)) {
    line = trim(std::move(line));
    if (line.empty()) {
      flush();
      continue;
    }
    if (line.rfind("physical id", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) phys = trim(line.substr(colon + 1));
    } else if (line.rfind("core id", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) core = trim(line.substr(colon + 1));
    }
  }
  flush();

  if (!unique.empty()) return std::to_string(unique.size());

  // Fallback: sockets * cpu cores.
  std::ifstream in2("/proc/cpuinfo");
  if (in2.is_open()) {
    std::unordered_set<std::string> sockets;
    sockets.reserve(8);
    int coresPerSocket = 0;
    std::string l;
    while (std::getline(in2, l)) {
      l = trim(std::move(l));
      if (l.rfind("physical id", 0) == 0) {
        const auto colon = l.find(':');
        if (colon != std::string::npos) sockets.insert(trim(l.substr(colon + 1)));
      } else if (l.rfind("cpu cores", 0) == 0 && coresPerSocket == 0) {
        const auto colon = l.find(':');
        if (colon != std::string::npos) {
          const std::string v = trim(l.substr(colon + 1));
          coresPerSocket = std::atoi(v.c_str());
        }
      }
    }
    if (!sockets.empty() && coresPerSocket > 0) {
      return std::to_string(static_cast<int>(sockets.size()) * coresPerSocket);
    }
  }

  return kUnknown;
}

static std::string normalizeSysfsSizeToken(std::string s) {
  // Common sysfs cache size formats: "32K", "256K", "16384K", "4M".
  s = trim(std::move(s));
  if (s.empty()) return std::string(kUnknown);
  return s;
}

static std::string probeCpuCacheL1() {
  namespace fs = std::filesystem;
  const fs::path base("/sys/devices/system/cpu/cpu0/cache");
  std::error_code ec;
  if (!fs::exists(base, ec)) return kUnknown;

  std::string l1d;
  std::string l1i;

  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (ec || !e.is_directory()) continue;
    const fs::path dir = e.path();
    const auto lvl = readTextFileTrim(dir / "level");
    const auto type = readTextFileTrim(dir / "type");
    const auto size = readTextFileTrim(dir / "size");
    if (!lvl || !type || !size) continue;
    if (*lvl != "1") continue;
    const std::string sz = normalizeSysfsSizeToken(*size);
    if (*type == "Data") l1d = sz;
    else if (*type == "Instruction") l1i = sz;
  }

  if (!l1d.empty() && !l1i.empty()) return l1d + " (d) + " + l1i + " (i)";
  if (!l1d.empty()) return l1d;
  if (!l1i.empty()) return l1i;
  return kUnknown;
}

static std::string probeCpuCacheLevel(const std::string& level) {
  namespace fs = std::filesystem;
  const fs::path base("/sys/devices/system/cpu/cpu0/cache");
  std::error_code ec;
  if (!fs::exists(base, ec)) return kUnknown;

  // Pick the largest cache size token we find for the requested level.
  std::string best;
  std::uint64_t bestBytes = 0;

  auto parseToBytes = [](const std::string& s) -> std::uint64_t {
    // Accept <num>[K|M] with optional spaces.
    std::string t = trim(s);
    if (t.empty()) return 0;
    char unit = 0;
    if (!t.empty()) {
      const char last = t.back();
      if (last == 'K' || last == 'M' || last == 'G') {
        unit = last;
        t.pop_back();
      }
    }
    t = trim(t);
    const std::uint64_t n = static_cast<std::uint64_t>(std::strtoull(t.c_str(), nullptr, 10));
    if (n == 0) return 0;
    if (unit == 'K') return n * 1024ull;
    if (unit == 'M') return n * 1024ull * 1024ull;
    if (unit == 'G') return n * 1024ull * 1024ull * 1024ull;
    return n;
  };

  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (ec || !e.is_directory()) continue;
    const fs::path dir = e.path();
    const auto lvl = readTextFileTrim(dir / "level");
    const auto size = readTextFileTrim(dir / "size");
    if (!lvl || !size) continue;
    if (*lvl != level) continue;
    const std::string sz = normalizeSysfsSizeToken(*size);
    const std::uint64_t b = parseToBytes(sz);
    if (b > bestBytes) {
      bestBytes = b;
      best = sz;
    }
  }

  if (!best.empty()) return best;
  return kUnknown;
}

static std::vector<std::string> probePerNicLinesLinux() {
  namespace fs = std::filesystem;
  const fs::path base("/sys/class/net");
  std::error_code ec;
  if (!fs::exists(base, ec)) return {};

  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (ec) break;
    const std::string n = e.path().filename().string();
    if (n.empty()) continue;
    if (n == "lo") continue;
    names.push_back(n);
  }
  std::sort(names.begin(), names.end());

  std::vector<std::string> lines;

  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& ifname = names[i];
    const fs::path p = base / ifname;

    std::string speedStr;
    if (const auto speed = readTextFileTrim(p / "speed")) {
      if (*speed != "-1") speedStr = *speed + " Mb/s";
    }

    std::string desc;
    const fs::path dev = p / "device";
    if (fs::exists(dev, ec)) {
      if (const auto slot = readUeventValue(dev / "uevent", "PCI_SLOT_NAME")) {
        if (const auto l = runCommand("lspci -s " + *slot + " 2>/dev/null | head -n 1")) {
          // lspci format: "04:00.0 Ethernet controller: Realtek ..."
          // Strip the leading bus id to keep it readable.
          const auto sp = l->find(' ');
          desc = (sp == std::string::npos) ? *l : l->substr(sp + 1);
        }
      }

      // If lspci isn't available (or doesn't help), fall back to driver name rather than raw hex IDs.
      if (desc.empty()) {
        if (const auto mod = symlinkBasename(dev / "driver" / "module")) {
          desc = *mod;
        } else if (const auto drv = symlinkBasename(dev / "driver")) {
          desc = *drv;
        }
      }

      if (desc.empty()) {
        // Last resort: interface name.
        desc = ifname;
      }
    }

    if (desc.empty()) desc = ifname;

    // One-line per NIC.
    std::ostringstream l;
    l << "NIC" << i << ": " << desc;
    if (!speedStr.empty()) l << " (" << speedStr << ")";
    lines.push_back(l.str());
  }

  return lines;
}

static std::vector<std::string> probePerDiskLinesLinux() {
  namespace fs = std::filesystem;
  const fs::path base("/sys/block");
  std::error_code ec;
  if (!fs::exists(base, ec)) return {};

  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (ec) break;
    const std::string n = e.path().filename().string();
    if (n.empty()) continue;
    if (n.rfind("loop", 0) == 0) continue;
    if (n.rfind("ram", 0) == 0) continue;
    names.push_back(n);
  }
  std::sort(names.begin(), names.end());

  std::vector<std::string> lines;

  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& devName = names[i];
    const fs::path p = base / devName;

    std::string model;
    if (const auto m = readTextFileTrim(p / "device" / "model")) {
      model = *m;
    } else if (const auto m = readTextFileTrim(p / "device" / "name")) {
      model = *m;
    }
    if (model.empty()) model = devName;

    const bool isNvme = (devName.rfind("nvme", 0) == 0);

    // Size
    std::uint64_t sectors = 0;
    if (const auto s = readTextFileTrim(p / "size")) {
      sectors = static_cast<std::uint64_t>(std::strtoull(s->c_str(), nullptr, 10));
    }
    std::uint64_t sectorSize = 512;
    if (const auto ss = readTextFileTrim(p / "queue" / "hw_sector_size")) {
      const std::uint64_t v = static_cast<std::uint64_t>(std::strtoull(ss->c_str(), nullptr, 10));
      if (v > 0) sectorSize = v;
    } else if (const auto ss = readTextFileTrim(p / "queue" / "logical_block_size")) {
      const std::uint64_t v = static_cast<std::uint64_t>(std::strtoull(ss->c_str(), nullptr, 10));
      if (v > 0) sectorSize = v;
    }

    std::string sizeG;
    if (sectors > 0) {
      const std::uint64_t bytes = sectors * sectorSize;
      const double g = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
      const long rounded = static_cast<long>(std::llround(g));
      if (std::abs(g - static_cast<double>(rounded)) < 0.05) {
        sizeG = std::to_string(rounded) + "G";
      } else {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(1);
        oss << g << "G";
        sizeG = oss.str();
      }
    }

    // One-line per disk.
    std::ostringstream l;
    l << "Disk" << i << ": ";
    if (isNvme) l << "NVMe ";
    l << model;
    if (!sizeG.empty()) l << " " << sizeG;
    lines.push_back(l.str());
  }

  return lines;
}

static std::string joinWith(const std::vector<std::string>& parts, const char* sep) {
  if (parts.empty()) return std::string();
  std::ostringstream oss;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) oss << sep;
    oss << parts[i];
  }
  return oss.str();
}

static std::vector<std::string> splitWs(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

static std::string probeCpuInstructions() {
  // Linux exposes supported ISA extensions via /proc/cpuinfo.
  // x86: "flags\t: ..."; ARM: "Features\t: ...".
  auto flagsLine = readFirstMatch("/proc/cpuinfo", "flags\t\t: ");
  if (!flagsLine) flagsLine = readFirstMatch("/proc/cpuinfo", "flags\t: ");
  if (!flagsLine) flagsLine = readFirstMatch("/proc/cpuinfo", "Features\t: ");
  if (!flagsLine) return kUnknown;

  const std::vector<std::string> tokens = splitWs(*flagsLine);
  if (tokens.empty()) return kUnknown;

  std::unordered_set<std::string> has;
  has.reserve(tokens.size());
  for (const auto& t : tokens) has.insert(t);

  struct Flag {
    const char* token;
    const char* label;
  };

  // Curated set of common ISA extensions; keep it short so it fits typical terminal widths.
  const Flag kFlags[] = {
      // x86 / x86_64
      {"mmx", "MMX"},
      {"sse", "SSE"},
      {"sse2", "SSE2"},
      {"sse3", "SSE3"},
      {"ssse3", "SSSE3"},
      {"sse4_1", "SSE4.1"},
      {"sse4_2", "SSE4.2"},
      {"popcnt", "POPCNT"},
      {"aes", "AES"},
      {"pclmulqdq", "PCLMUL"},
      {"fma", "FMA"},
      {"f16c", "F16C"},
      {"avx", "AVX"},
      {"avx2", "AVX2"},
      {"bmi1", "BMI1"},
      {"bmi2", "BMI2"},
      {"avx512f", "AVX-512F"},
      {"avx512bw", "AVX-512BW"},
      {"avx512dq", "AVX-512DQ"},
      {"avx512vl", "AVX-512VL"},
      {"avx512cd", "AVX-512CD"},
      {"avx512vnni", "AVX-512VNNI"},
      // ARM (AArch64 typically)
      {"neon", "NEON"},
      {"asimd", "NEON"},
      {"fp", "FP"},
      {"asimdhp", "FP16"},
      {"crc32", "CRC32"},
      {"sha1", "SHA1"},
      {"sha2", "SHA2"},
      {"sha3", "SHA3"},
      {"sm3", "SM3"},
      {"sm4", "SM4"},
      {"sve", "SVE"},
      {"sve2", "SVE2"},
  };

  std::vector<std::string> out;
  out.reserve(sizeof(kFlags) / sizeof(kFlags[0]));

  for (const auto& f : kFlags) {
    if (has.find(f.token) != has.end()) {
      // Avoid duplicate labels (e.g. neon + asimd).
      if (out.empty() || out.back() != f.label) out.push_back(f.label);
    }
  }

  if (out.empty()) return kUnknown;
  return joinWith(out, " ");
}

static std::string probeRamSummary() {
  // Always provide total RAM from /proc/meminfo.
  std::ifstream in("/proc/meminfo");
  if (!in.is_open()) return kUnknown;

  std::string line;
  std::uint64_t memTotalKb = 0;
  while (std::getline(in, line)) {
    if (line.rfind("MemTotal:", 0) == 0) {
      std::istringstream iss(line);
      std::string key;
      iss >> key >> memTotalKb;
      break;
    }
  }

  double gib = static_cast<double>(memTotalKb) / (1024.0 * 1024.0);
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << gib << " GiB";

  // Best-effort: try dmidecode for speed/channels (may require root).
  // We keep this optional and gracefully degrade.
  // Speed: take the max configured speed we find. Channels: try to infer from number of populated DIMMs.
  const auto dmi = runCommand("dmidecode -t memory 2>/dev/null");
  if (!dmi) {
    oss << " (speed: --, channels: --)";
    return oss.str();
  }

  std::istringstream dmiStream(*dmi);
  std::string dmiLine;
  int populatedDimms = 0;
  int speedsFound = 0;
  int maxMhz = 0;
  std::string ramType;

  while (std::getline(dmiStream, dmiLine)) {
    dmiLine = trim(dmiLine);
    if (ramType.empty() && dmiLine.rfind("Type:", 0) == 0) {
      // Examples: "Type: DDR4", "Type: LPDDR5", "Type: Unknown"
      std::string t = trim(dmiLine.substr(std::string("Type:").size()));
      if (!t.empty() && t != "Unknown" && t != "Other") {
        // Prefer DDR*/LPDDR* type tokens.
        if (t.rfind("DDR", 0) == 0 || t.rfind("LPDDR", 0) == 0) {
          ramType = t;
        }
      }
    }
    if (dmiLine.rfind("Size:", 0) == 0) {
      if (dmiLine.find("No Module Installed") == std::string::npos) {
        ++populatedDimms;
      }
    }
    if (dmiLine.rfind("Configured Memory Speed:", 0) == 0 || dmiLine.rfind("Speed:", 0) == 0) {
      // Examples: "Speed: 3200 MT/s" or "Configured Memory Speed: 2933 MT/s"
      // Parse the first integer.
      std::istringstream iss(dmiLine);
      std::string tmp;
      while (iss >> tmp) {
        bool allDigits = !tmp.empty();
        for (char c : tmp) allDigits = allDigits && std::isdigit(static_cast<unsigned char>(c));
        if (allDigits) {
          const int mhz = std::atoi(tmp.c_str());
          if (mhz > 0) {
            ++speedsFound;
            if (mhz > maxMhz) maxMhz = mhz;
          }
          break;
        }
      }
    }
  }

  if (!ramType.empty()) {
    oss << " " << ramType;
  }

  oss << " (speed: ";
  if (speedsFound > 0) oss << maxMhz << " MT/s";
  else oss << kUnknown;

  // Channels are not reliably inferable from dmidecode alone; provide a conservative hint.
  // If 2 DIMMs populated, it's often dual-channel, but not guaranteed.
  oss << ", channels: ";
  if (populatedDimms >= 2) oss << "likely >=2";
  else if (populatedDimms == 1) oss << "likely 1";
  else oss << kUnknown;

  oss << ")";
  return oss.str();
}

static std::string probeGpuName() {
  // Prefer NVML when available (no external tools).
  if (auto n = readNvmlGpuNameForGpu(0)) return *n;

  // Fallback: lspci line for VGA/3D.
  if (auto l = runCommand("lspci 2>/dev/null | grep -E 'VGA|3D|Display' | head -n 1")) return *l;

  return kUnknown;
}

static std::string probeGpuDriver() {
  // NVIDIA: NVML provides driver version.
  if (auto v = readNvmlDriverVersion()) return "nvidia " + *v;

  // AMD/Intel: kernel module version via modinfo (often equals kernel version string).
  if (auto v = runCommand("modinfo -F version amdgpu 2>/dev/null | head -n 1")) return "amdgpu " + *v;
  if (auto v = runCommand("modinfo -F version i915 2>/dev/null | head -n 1")) return "i915 " + *v;
  if (auto v = runCommand("modinfo -F version xe 2>/dev/null | head -n 1")) return "xe " + *v;

  return kUnknown;
}

static std::pair<std::string, std::vector<std::string>> probeNpuInfo() {
  std::string summary;
  std::vector<std::string> lines;

  auto npuResult = probeNpuDevices();

  if (npuResult.status != NpuStatus::Available || npuResult.devices.empty()) {
    return {kUnknown, {}};
  }

  // Build summary from first device
  if (!npuResult.devices.empty()) {
    summary = npuResult.devices[0].name;
    if (npuResult.devices.size() > 1) {
      summary += " (+" + std::to_string(npuResult.devices.size() - 1) + " more)";
    }
  }

  // Build per-NPU lines
  for (std::size_t i = 0; i < npuResult.devices.size(); ++i) {
    const auto& npu = npuResult.devices[i];
    lines.push_back("NPU" + std::to_string(i) + ": " + npu.name);
    for (const auto& detail : npu.detailLines) {
      lines.push_back(detail);
    }
  }

  return {summary, lines};
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  const bool hasPerNic = !perNicLines.empty();
  const bool hasPerDisk = !perDiskLines.empty();
  const std::string indent = " ";
  std::vector<std::string> lines;
  lines.reserve(48);

  lines.push_back("OS: " + (osPretty.empty() ? std::string(kUnknown) : osPretty));
  lines.push_back("Kernel: " + (kernelVersion.empty() ? std::string(kUnknown) : kernelVersion));

  // Requested: these should be under OS/KERNEL and before CPU.
  lines.push_back("GPU Driver: " + (gpuDriver.empty() ? std::string(kUnknown) : gpuDriver));
  lines.push_back("CUDA: " + (cudaVersion.empty() ? std::string(kUnknown) : cudaVersion));
  lines.push_back("NVML: " + (nvmlVersion.empty() ? std::string(kUnknown) : nvmlVersion));
  lines.push_back("ROCm: " + (rocmVersion.empty() ? std::string(kUnknown) : rocmVersion));
  lines.push_back("OpenCL: " + (openclVersion.empty() ? std::string(kUnknown) : openclVersion));
  lines.push_back("Vulkan: " + (vulkanVersion.empty() ? std::string(kUnknown) : vulkanVersion));

  // Requested: a space after those before CPU.
  lines.push_back(std::string());

  lines.push_back("CPU: " + (cpuName.empty() ? std::string(kUnknown) : cpuName));
  lines.push_back(indent + "Physical cores: " + (cpuPhysicalCores.empty() ? std::string(kUnknown) : cpuPhysicalCores));
  lines.push_back(indent + "Logical cores: " + (cpuLogicalCores.empty() ? std::string(kUnknown) : cpuLogicalCores));
  lines.push_back(indent + "Cache L1: " + (cpuCacheL1.empty() ? std::string(kUnknown) : cpuCacheL1));
  lines.push_back(indent + "Cache L2: " + (cpuCacheL2.empty() ? std::string(kUnknown) : cpuCacheL2));
  lines.push_back(indent + "Cache L3: " + (cpuCacheL3.empty() ? std::string(kUnknown) : cpuCacheL3));
  lines.push_back(indent + "Instructions: " + (cpuInstructions.empty() ? std::string(kUnknown) : cpuInstructions));
  lines.push_back(indent + "RAM: " + (ramSummary.empty() ? std::string(kUnknown) : ramSummary));

  // GPU hardware details after CPU/RAM.
  lines.push_back(std::string());

  if (!hasPerGpu) {
    lines.push_back("GPU: " + (gpuName.empty() ? std::string(kUnknown) : gpuName));
    lines.push_back("Memory: " + (vramSummary.empty() ? std::string(kUnknown) : vramSummary));
  } else {
    for (const auto& l : perGpuLines) lines.push_back(l);
  }

  // NPU (Neural Processing Unit) details after GPU.
  const bool hasPerNpu = !perNpuLines.empty();
  if (hasPerNpu) {
    lines.push_back(std::string());
    for (const auto& l : perNpuLines) lines.push_back(l);
  } else if (!npuSummary.empty() && npuSummary != kUnknown) {
    lines.push_back(std::string());
    lines.push_back("NPU: " + npuSummary);
  }

  // Vulkan is shown in the OS/Kernel block.

  // NICs/Disks go at the end; each entry is one line.
  if (hasPerNic || hasPerDisk) lines.push_back(std::string());
  for (const auto& l : perNicLines) lines.push_back(l);
  for (const auto& l : perDiskLines) lines.push_back(l);

  return lines;
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = readOsPrettyName().value_or(kUnknown);
  info.kernelVersion = probeKernelVersion();
  info.cpuName = probeCpuName();
  info.cpuPhysicalCores = probeCpuPhysicalCores();
  info.cpuLogicalCores = probeCpuLogicalCores();
  info.cpuCacheL1 = probeCpuCacheL1();
  info.cpuCacheL2 = probeCpuCacheLevel("2");
  info.cpuCacheL3 = probeCpuCacheLevel("3");
  info.cpuInstructions = probeCpuInstructions();
  info.ramSummary = probeRamSummary();
  info.gpuName = probeGpuName();
  info.gpuDriver = probeGpuDriver();
  info.perGpuLines = probePerGpuLinesNvidia();
  info.perNicLines = probePerNicLinesLinux();
  info.perDiskLines = probePerDiskLinesLinux();
  info.vramSummary = probeVramSummary();
  info.cudaVersion = probeCudaVersion();
  info.nvmlVersion = probeNvmlVersion();
  info.rocmVersion = probeRocmVersion();
  info.openclVersion = probeOpenCLVersion();
  info.vulkanVersion = probeVulkanVersion();

  // Probe NPU (Intel/AMD Neural Processing Units)
  auto [npuSummary, npuLines] = probeNpuInfo();
  info.npuSummary = std::move(npuSummary);
  info.perNpuLines = std::move(npuLines);

  return info;
}

}  // namespace aiz
