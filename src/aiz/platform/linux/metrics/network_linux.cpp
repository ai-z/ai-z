#include <aiz/platform/metrics/network.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace aiz::platform {

static std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::optional<NetworkCounters> readNetworkCounters(const std::string& interfaceFilter) {
  std::ifstream in("/proc/net/dev");
  if (!in.is_open()) return std::nullopt;

  std::string line;
  std::getline(in, line);
  std::getline(in, line);

  std::uint64_t rxTotal = 0;
  std::uint64_t txTotal = 0;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string iface = trim(line.substr(0, colon));
    std::string rest = line.substr(colon + 1);

    if (iface.empty()) continue;

    if (!interfaceFilter.empty()) {
      if (iface.rfind(interfaceFilter, 0) != 0) continue;
    } else {
      if (iface == "lo") continue;
    }

    std::istringstream iss(rest);
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;

    iss >> rxBytes;
    for (int i = 0; i < 7; ++i) {
      std::uint64_t dummy = 0;
      iss >> dummy;
    }

    iss >> txBytes;
    rxTotal += rxBytes;
    txTotal += txBytes;
  }

  return NetworkCounters{rxTotal, txTotal};
}

}  // namespace aiz::platform
