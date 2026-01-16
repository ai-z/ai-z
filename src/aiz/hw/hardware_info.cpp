#include <aiz/hw/hardware_info.h>

#if defined(_WIN32)

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <windows.h>

namespace aiz {

static std::string trim(std::string s) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
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
  if (!ntdll) return "unknown";

  auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtlGetVersion) return "unknown";

  RTL_OSVERSIONINFOW ver{};
  ver.dwOSVersionInfoSize = sizeof(ver);
  if (rtlGetVersion(&ver) != 0) return "unknown";

  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", static_cast<unsigned long>(ver.dwMajorVersion),
                static_cast<unsigned long>(ver.dwMinorVersion), static_cast<unsigned long>(ver.dwBuildNumber));
  return std::string(buf);
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  std::vector<std::string> lines = {
      "OS: " + (osPretty.empty() ? std::string("unknown") : osPretty),
      "Kernel: " + (kernelVersion.empty() ? std::string("unknown") : kernelVersion),
      "CPU: " + (cpuName.empty() ? std::string("unknown") : cpuName),
      "CPU Instructions: " + (cpuInstructions.empty() ? std::string("unknown") : cpuInstructions),
      "RAM: " + (ramSummary.empty() ? std::string("unknown") : ramSummary),
      // If we have per-GPU lines, the single GPU line is redundant.
      hasPerGpu ? std::string() : ("GPU: " + (gpuName.empty() ? std::string("unknown") : gpuName)),
      "GPU Driver: " + (gpuDriver.empty() ? std::string("unknown") : gpuDriver),
  };

  // Drop any empty placeholders (e.g., the omitted GPU line).
  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  for (const auto& l : perGpuLines) lines.push_back(l);

    lines.insert(lines.end(), {
      // If we have per-GPU lines, VRAM is already shown per GPU.
      hasPerGpu ? std::string() : ("VRAM: " + (vramSummary.empty() ? std::string("unknown") : vramSummary)),
      "CUDA: " + (cudaVersion.empty() ? std::string("unknown") : cudaVersion),
      "NVML: " + (nvmlVersion.empty() ? std::string("unknown") : nvmlVersion),
      "ROCm: " + (rocmVersion.empty() ? std::string("unknown") : rocmVersion),
      "OpenGL: " + (openglVersion.empty() ? std::string("unknown") : openglVersion),
      "Vulkan: " + (vulkanVersion.empty() ? std::string("unknown") : vulkanVersion),
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
    info.cpuName = "unknown";
  }
  info.cpuInstructions = "unknown";
  info.ramSummary = "unknown";
  info.gpuName = "unknown";
  info.gpuDriver = "unknown";
  info.vramSummary = "unknown";
  info.cudaVersion = runCommand("nvidia-smi 2>nul | findstr /C:\"CUDA Version\"")
                         .value_or("unknown");
  info.nvmlVersion = runCommand("nvidia-smi -q 2>nul | findstr /C:\"NVML Version\"")
                         .value_or("unknown");
  info.rocmVersion = "unknown";
  info.openglVersion = "unknown";
  info.vulkanVersion = "unknown";
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
#include <fstream>
#include <iomanip>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <string>

#include <aiz/metrics/nvidia_nvml.h>

namespace aiz {

static std::string trim(std::string s) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
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
  return runCommand("uname -r 2>/dev/null").value_or("unknown");
}

static std::string probeCudaVersion() {
  // nvidia-smi output usually contains "CUDA Version: X.Y".
  const auto v = runCommand(
      "nvidia-smi 2>/dev/null | grep -o 'CUDA Version: [0-9.]*' | head -n 1 | awk '{print $3}'");
  return v.value_or("unknown");
}

static std::string probeNvmlVersion() {
  const auto v = runCommand(
      "nvidia-smi -q 2>/dev/null | awk -F: '/NVML Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'");
  return v.value_or("unknown");
}

static std::string probeVramSummary() {
  // NVIDIA: sum memory.total across all GPUs (MiB).
  const auto out = runCommand(
      "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null");
  if (!out) return "unknown";

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

  if (gpus <= 0 || sumMiB == 0) return "unknown";
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
  return "unknown";
}

static std::string probeOpenGLVersion() {
  // Requires mesa-utils (glxinfo). If absent, returns unknown.
  const auto v = runCommand(
      "glxinfo -B 2>/dev/null | awk -F: '/OpenGL version string/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'");
  return v.value_or("unknown");
}

static std::string probeVulkanVersion() {
  // Requires vulkan-tools (vulkaninfo). If absent, returns unknown.
  if (auto v = runCommand(
          "vulkaninfo --summary 2>/dev/null | awk -F: '/Vulkan Instance Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return *v;
  }
  if (auto v = runCommand(
          "vulkaninfo 2>/dev/null | awk -F: '/Vulkan Instance Version/{gsub(/^[ \\t]+/,\"\",$2); print $2; exit}'")) {
    return *v;
  }
  return "unknown";
}

static std::string probeCpuName() {
  auto v = readFirstMatch("/proc/cpuinfo", "model name\t: ");
  if (v) return *v;
  v = readFirstMatch("/proc/cpuinfo", "Hardware\t: ");
  if (v) return *v;
  return "unknown";
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
  if (!flagsLine) return "unknown";

  const std::vector<std::string> tokens = splitWs(*flagsLine);
  if (tokens.empty()) return "unknown";

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

  if (out.empty()) return "unknown";
  return joinWith(out, " ");
}

static std::string probeRamSummary() {
  // Always provide total RAM from /proc/meminfo.
  std::ifstream in("/proc/meminfo");
  if (!in.is_open()) return "unknown";

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
    oss << " (speed: unknown, channels: unknown)";
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
  else oss << "unknown";

  // Channels are not reliably inferable from dmidecode alone; provide a conservative hint.
  // If 2 DIMMs populated, it's often dual-channel, but not guaranteed.
  oss << ", channels: ";
  if (populatedDimms >= 2) oss << "likely >=2";
  else if (populatedDimms == 1) oss << "likely 1";
  else oss << "unknown";

  oss << ")";
  return oss.str();
}

static std::string probeGpuName() {
  // Prefer vendor tools when available.
  if (auto n = runCommand("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -n 1")) return *n;
  if (auto a = runCommand("rocm-smi --showproductname 2>/dev/null | head -n 1")) return *a;

  // Fallback: lspci line for VGA/3D.
  if (auto l = runCommand("lspci 2>/dev/null | grep -E 'VGA|3D|Display' | head -n 1")) return *l;

  return "unknown";
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

  return "unknown";
}

std::vector<std::string> HardwareInfo::toLines() const {
  const bool hasPerGpu = !perGpuLines.empty();
  std::vector<std::string> lines = {
      "OS: " + (osPretty.empty() ? std::string("unknown") : osPretty),
      "Kernel: " + (kernelVersion.empty() ? std::string("unknown") : kernelVersion),
      "CPU: " + (cpuName.empty() ? std::string("unknown") : cpuName),
      "CPU Instructions: " + (cpuInstructions.empty() ? std::string("unknown") : cpuInstructions),
      "RAM: " + (ramSummary.empty() ? std::string("unknown") : ramSummary),
      hasPerGpu ? std::string() : ("GPU: " + (gpuName.empty() ? std::string("unknown") : gpuName)),
      "GPU Driver: " + (gpuDriver.empty() ? std::string("unknown") : gpuDriver),
  };

  lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  for (const auto& l : perGpuLines) lines.push_back(l);

    lines.insert(lines.end(), {
      hasPerGpu ? std::string() : ("VRAM: " + (vramSummary.empty() ? std::string("unknown") : vramSummary)),
      "CUDA: " + (cudaVersion.empty() ? std::string("unknown") : cudaVersion),
      "NVML: " + (nvmlVersion.empty() ? std::string("unknown") : nvmlVersion),
      "ROCm: " + (rocmVersion.empty() ? std::string("unknown") : rocmVersion),
      "OpenGL: " + (openglVersion.empty() ? std::string("unknown") : openglVersion),
      "Vulkan: " + (vulkanVersion.empty() ? std::string("unknown") : vulkanVersion),
  });

    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& s) { return s.empty(); }), lines.end());

  return lines;
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = readOsPrettyName().value_or("unknown");
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
  info.openglVersion = probeOpenGLVersion();
  info.vulkanVersion = probeVulkanVersion();
  return info;
}

}  // namespace aiz

#endif
