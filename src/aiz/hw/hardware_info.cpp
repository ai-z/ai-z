#include <aiz/hw/hardware_info.h>

#if defined(_WIN32)

#include <cstdlib>

namespace aiz {

std::vector<std::string> HardwareInfo::toLines() const {
  return {
      "OS: " + (osPretty.empty() ? std::string("unknown") : osPretty),
      "CPU: " + (cpuName.empty() ? std::string("unknown") : cpuName),
      "RAM: " + (ramSummary.empty() ? std::string("unknown") : ramSummary),
      "GPU: " + (gpuName.empty() ? std::string("unknown") : gpuName),
      "GPU Driver: " + (gpuDriver.empty() ? std::string("unknown") : gpuDriver),
  };
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = "Windows";
  if (const char* cpu = std::getenv("PROCESSOR_IDENTIFIER")) {
    info.cpuName = cpu;
  } else {
    info.cpuName = "unknown";
  }
  info.ramSummary = "unknown";
  info.gpuName = "unknown";
  info.gpuDriver = "unknown";
  return info;
}

}  // namespace aiz

#else

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

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

static std::string probeCpuName() {
  auto v = readFirstMatch("/proc/cpuinfo", "model name\t: ");
  if (v) return *v;
  v = readFirstMatch("/proc/cpuinfo", "Hardware\t: ");
  if (v) return *v;
  return "unknown";
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
  return {
      "OS: " + (osPretty.empty() ? std::string("unknown") : osPretty),
      "CPU: " + (cpuName.empty() ? std::string("unknown") : cpuName),
      "RAM: " + (ramSummary.empty() ? std::string("unknown") : ramSummary),
      "GPU: " + (gpuName.empty() ? std::string("unknown") : gpuName),
      "GPU Driver: " + (gpuDriver.empty() ? std::string("unknown") : gpuDriver),
  };
}

HardwareInfo HardwareInfo::probe() {
  HardwareInfo info;
  info.osPretty = readOsPrettyName().value_or("unknown");
  info.cpuName = probeCpuName();
  info.ramSummary = probeRamSummary();
  info.gpuName = probeGpuName();
  info.gpuDriver = probeGpuDriver();
  return info;
}

}  // namespace aiz

#endif
