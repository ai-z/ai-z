#include <aiz/hw/hardware_info.h>

#if defined(_WIN32)

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

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

  // Common format: "<major>.<minor> <impl...>" (e.g. "3.0 CUDA 13.0.94").
  // Make it explicit that the suffix is the implementation/driver string.
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

static std::optional<std::string> runCommand(const std::string& cmd) {
  std::array<char, 4096> buf{};
  std::string out;

  FILE* pipe = _popen(cmd.c_str(), "r");
  if (!pipe) return std::nullopt;

  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
    if (out.size() > 64 * 1024) break;
  }

  const int rc = _pclose(pipe);
  if (rc != 0) return std::nullopt;

  out = trim(out);
  if (out.empty()) return std::nullopt;
  return out;
}

static std::string probeWindowsKernelVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return kUnknown;

  auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtlGetVersion) return kUnknown;

  RTL_OSVERSIONINFOW ver{};
  ver.dwOSVersionInfoSize = sizeof(ver);
  if (rtlGetVersion(&ver) != 0) return kUnknown;

  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", static_cast<unsigned long>(ver.dwMajorVersion),
                static_cast<unsigned long>(ver.dwMinorVersion), static_cast<unsigned long>(ver.dwBuildNumber));
  return std::string(buf);
}

static std::string probeOpenCLVersion() {
  // Use OpenCL ICD loader if present.
  // We intentionally avoid including OpenCL headers to keep the dependency optional.
  using cl_int = int;
  using cl_uint = unsigned int;
  using cl_platform_id = void*;
  constexpr cl_int CL_SUCCESS = 0;
  constexpr cl_uint CL_PLATFORM_VERSION = 0x0901;

  using clGetPlatformIDs_t = cl_int (*)(cl_uint, cl_platform_id*, cl_uint*);
  using clGetPlatformInfo_t = cl_int (*)(cl_platform_id, cl_uint, std::size_t, void*, std::size_t*);

  HMODULE lib = LoadLibraryA("OpenCL.dll");
  if (!lib) return kUnknown;

  auto clGetPlatformIDs = reinterpret_cast<clGetPlatformIDs_t>(GetProcAddress(lib, "clGetPlatformIDs"));
  auto clGetPlatformInfo = reinterpret_cast<clGetPlatformInfo_t>(GetProcAddress(lib, "clGetPlatformInfo"));
  if (!clGetPlatformIDs || !clGetPlatformInfo) {
    FreeLibrary(lib);
    return kUnknown;
  }

  cl_uint n = 0;
  if (clGetPlatformIDs(0, nullptr, &n) != CL_SUCCESS || n == 0) {
    FreeLibrary(lib);
    return kUnknown;
  }

  // Read version string for the first platform.
  cl_platform_id pid = nullptr;
  if (clGetPlatformIDs(1, &pid, nullptr) != CL_SUCCESS || !pid) {
    FreeLibrary(lib);
    return kUnknown;
  }

  std::size_t size = 0;
  if (clGetPlatformInfo(pid, CL_PLATFORM_VERSION, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
    FreeLibrary(lib);
    return kUnknown;
  }

  std::string s;
  s.resize(size);
  if (clGetPlatformInfo(pid, CL_PLATFORM_VERSION, size, s.data(), nullptr) != CL_SUCCESS) {
    FreeLibrary(lib);
    return kUnknown;
  }
  FreeLibrary(lib);

  // Trim at NUL and whitespace.
  const auto nul = s.find('\0');
  if (nul != std::string::npos) s.resize(nul);
  return normalizeOpenCLPlatformVersion(std::move(s));
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  std::vector<std::string> lines = {
      "OS: " + (osPretty.empty() ? std::string(kUnknown) : osPretty),
      "Kernel: " + (kernelVersion.empty() ? std::string(kUnknown) : kernelVersion),
      "CPU: " + (cpuName.empty() ? std::string(kUnknown) : cpuName),
      "CPU Instructions: " + (cpuInstructions.empty() ? std::string(kUnknown) : cpuInstructions),
      "RAM: " + (ramSummary.empty() ? std::string(kUnknown) : ramSummary),
      // If we have per-GPU lines, the single GPU line is redundant.
      hasPerGpu ? std::string() : ("GPU: " + (gpuName.empty() ? std::string(kUnknown) : gpuName)),
      "GPU Driver: " + (gpuDriver.empty() ? std::string(kUnknown) : gpuDriver),
  };

  // Drop any empty placeholders (e.g., the omitted GPU line).
  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  for (const auto& l : perGpuLines) lines.push_back(l);

  lines.insert(lines.end(), {
      // If we have per-GPU lines, VRAM is already shown per GPU.
      hasPerGpu ? std::string() : ("VRAM: " + (vramSummary.empty() ? std::string(kUnknown) : vramSummary)),
      "CUDA: " + (cudaVersion.empty() ? std::string(kUnknown) : cudaVersion),
      "NVML: " + (nvmlVersion.empty() ? std::string(kUnknown) : nvmlVersion),
      "ROCm: " + (rocmVersion.empty() ? std::string(kUnknown) : rocmVersion),
        "OpenCL: " + (openclVersion.empty() ? std::string(kUnknown) : openclVersion),
      "Vulkan: " + (vulkanVersion.empty() ? std::string(kUnknown) : vulkanVersion),
  });

  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  return lines;
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = "Windows";
  info.kernelVersion = probeWindowsKernelVersion();
  if (const char* cpu = std::getenv("PROCESSOR_IDENTIFIER")) {
    info.cpuName = cpu;
  } else {
    info.cpuName = kUnknown;
  }
  info.cpuInstructions = kUnknown;
  info.ramSummary = kUnknown;
  info.gpuName = kUnknown;
  info.gpuDriver = kUnknown;
  info.vramSummary = kUnknown;
  info.cudaVersion = runCommand("nvidia-smi 2>nul | findstr /C:\"CUDA Version\"")
                         .value_or(kUnknown);
  info.nvmlVersion = runCommand("nvidia-smi -q 2>nul | findstr /C:\"NVML Version\"")
                         .value_or(kUnknown);
  info.rocmVersion = kUnknown;
  info.openclVersion = probeOpenCLVersion();
  // Best-effort: if vulkaninfo is installed, try to read the instance version.
  info.vulkanVersion = runCommand(
      "vulkaninfo --summary 2>nul | findstr /C:\"Vulkan Instance Version\"")
          .value_or(kUnknown);
  return info;
}

}  // namespace aiz

#else

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <string>

#include <dlfcn.h>

#include <aiz/metrics/nvidia_nvml.h>

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

static std::string fmtGiBFromMiB(std::uint64_t mib) {
  const double gib = static_cast<double>(mib) / 1024.0;
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << gib << " GiB";
  return oss.str();
}

static std::string fmtGFromMiB(std::uint64_t mib) {
  const double g = static_cast<double>(mib) / 1024.0;
  const long rounded = static_cast<long>(std::llround(g));
  if (std::abs(g - static_cast<double>(rounded)) < 0.05) {
    return std::to_string(rounded) + "G";
  }
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(1);
  oss << g << "G";
  return oss.str();
}

static std::vector<std::string> probePerGpuLinesNvidia() {
  // Example output rows (csv,noheader,nounits):
  // 0, NVIDIA GeForce RTX 4090, 24220
  const auto out = runCommand(
      "nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader,nounits 2>/dev/null");
  if (!out) return {};

  // Match the user's requested formatting:
  // GPU0: <name>
  //            VRAM: 10G
  const std::string indent = " ";
  std::vector<std::string> lines;
  std::istringstream iss(*out);
  std::string line;
  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) continue;

    // Split by commas.
    std::string a, b, c;
    {
      std::istringstream ls(line);
      if (!std::getline(ls, a, ',')) continue;
      if (!std::getline(ls, b, ',')) continue;
      if (!std::getline(ls, c, ',')) continue;
    }
    a = trim(a);
    b = trim(b);
    c = trim(c);

    std::uint64_t mib = 0;
    {
      std::istringstream cs(c);
      cs >> mib;
    }
    if (a.empty() || b.empty() || mib == 0) continue;

    unsigned int gpuIndex = 0;
    try {
      gpuIndex = static_cast<unsigned int>(std::stoul(a));
    } catch (...) {
      continue;
    }

    {
      std::ostringstream l;
      l << "GPU" << a << ": " << b;
      lines.push_back(l.str());
    }
    {
      std::ostringstream l;
      l << indent << "VRAM: " << fmtGFromMiB(mib);
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
  // nvidia-smi output usually contains "CUDA Version: X.Y".
  const auto v = runCommand(
      "nvidia-smi 2>/dev/null | grep -o 'CUDA Version: [0-9.]*' | head -n 1 | awk '{print $3}'");
  return v.value_or(kUnknown);
}

static std::string probeNvmlVersion() {
  if (auto v = readNvmlLibraryVersion()) return *v;
  const auto v = runCommand(
      "nvidia-smi -q 2>/dev/null | awk -F: '/NVML Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'");
  return v.value_or(kUnknown);
}

static std::string probeVramSummary() {
  // NVIDIA: sum memory.total across all GPUs (MiB).
  const auto out = runCommand(
      "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null");
  if (!out) return kUnknown;

  std::istringstream iss(*out);
  std::string line;
  std::uint64_t sumMiB = 0;
  int gpus = 0;
  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) continue;
    std::uint64_t mib = 0;
    std::istringstream ls(line);
    ls >> mib;
    if (mib == 0) continue;
    sumMiB += mib;
    ++gpus;
  }

  if (gpus <= 0 || sumMiB == 0) return kUnknown;
  if (gpus == 1) return fmtGiBFromMiB(sumMiB);

  std::ostringstream oss;
  oss << fmtGiBFromMiB(sumMiB) << " (" << gpus << " GPUs)";
  return oss.str();
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

  while (std::getline(dmiStream, dmiLine)) {
    dmiLine = trim(dmiLine);
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
  // Prefer vendor tools when available.
  if (auto n = runCommand("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -n 1")) return *n;
  if (auto a = runCommand("rocm-smi --showproductname 2>/dev/null | head -n 1")) return *a;

  // Fallback: lspci line for VGA/3D.
  if (auto l = runCommand("lspci 2>/dev/null | grep -E 'VGA|3D|Display' | head -n 1")) return *l;

  return kUnknown;
}

static std::string probeGpuDriver() {
  // NVIDIA: nvidia-smi provides driver version.
  if (auto v = runCommand("nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n 1")) {
    return "nvidia " + *v;
  }

  // AMD/Intel: kernel module version via modinfo (often equals kernel version string).
  if (auto v = runCommand("modinfo -F version amdgpu 2>/dev/null | head -n 1")) return "amdgpu " + *v;
  if (auto v = runCommand("modinfo -F version i915 2>/dev/null | head -n 1")) return "i915 " + *v;
  if (auto v = runCommand("modinfo -F version xe 2>/dev/null | head -n 1")) return "xe " + *v;

  return kUnknown;
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  std::vector<std::string> lines = {
      "OS: " + (osPretty.empty() ? std::string(kUnknown) : osPretty),
      "Kernel: " + (kernelVersion.empty() ? std::string(kUnknown) : kernelVersion),
      "CPU: " + (cpuName.empty() ? std::string(kUnknown) : cpuName),
      "CPU Instructions: " + (cpuInstructions.empty() ? std::string(kUnknown) : cpuInstructions),
      "RAM: " + (ramSummary.empty() ? std::string(kUnknown) : ramSummary),
      hasPerGpu ? std::string() : ("GPU: " + (gpuName.empty() ? std::string(kUnknown) : gpuName)),
      "GPU Driver: " + (gpuDriver.empty() ? std::string(kUnknown) : gpuDriver),
  };

  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  for (const auto& l : perGpuLines) lines.push_back(l);

  lines.insert(lines.end(), {
      hasPerGpu ? std::string() : ("VRAM: " + (vramSummary.empty() ? std::string(kUnknown) : vramSummary)),
      "CUDA: " + (cudaVersion.empty() ? std::string(kUnknown) : cudaVersion),
      "NVML: " + (nvmlVersion.empty() ? std::string(kUnknown) : nvmlVersion),
      "ROCm: " + (rocmVersion.empty() ? std::string(kUnknown) : rocmVersion),
        "OpenCL: " + (openclVersion.empty() ? std::string(kUnknown) : openclVersion),
      "Vulkan: " + (vulkanVersion.empty() ? std::string(kUnknown) : vulkanVersion),
  });

  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  return lines;
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = readOsPrettyName().value_or(kUnknown);
  info.kernelVersion = probeKernelVersion();
  info.cpuName = probeCpuName();
  info.cpuInstructions = probeCpuInstructions();
  info.ramSummary = probeRamSummary();
  info.gpuName = probeGpuName();
  info.gpuDriver = probeGpuDriver();
  info.perGpuLines = probePerGpuLinesNvidia();
  info.vramSummary = probeVramSummary();
  info.cudaVersion = probeCudaVersion();
  info.nvmlVersion = probeNvmlVersion();
  info.rocmVersion = probeRocmVersion();
  info.openclVersion = probeOpenCLVersion();
  info.vulkanVersion = probeVulkanVersion();
  return info;
}

}  // namespace aiz

#endif
