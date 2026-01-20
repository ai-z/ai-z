#include <aiz/metrics/network_bandwidth.h>
#include <cctype>
#include <fstream>
#include <sstream>

namespace aiz {

NetworkBandwidthCollector::NetworkBandwidthCollector(NetworkBandwidthMode mode, std::string ifacePrefix)
    : mode_(mode), ifacePrefix_(std::move(ifacePrefix)) {}

NetworkBandwidthCollector::~NetworkBandwidthCollector() = default;

static std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static bool readNetworkBytesTotal(NetworkBandwidthMode mode, const std::string& ifacePrefix, std::uint64_t& bytesOut) {
  // /proc/net/dev format:
  // Inter-|   Receive                                                |  Transmit
  //  face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
  std::ifstream in("/proc/net/dev");
  if (!in.is_open()) return false;

  std::string line;
  // Skip 2 header lines.
  std::getline(in, line);
  std::getline(in, line);

  std::uint64_t total = 0;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string iface = trim(line.substr(0, colon));
    std::string rest = line.substr(colon + 1);

    if (iface.empty()) continue;

    if (!ifacePrefix.empty()) {
      if (iface.rfind(ifacePrefix, 0) != 0) continue;
    } else {
      // Default: ignore loopback.
      if (iface == "lo") continue;
    }

    std::istringstream iss(rest);
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;

    // rx: bytes packets errs drop fifo frame compressed multicast
    iss >> rxBytes;
    for (int i = 0; i < 7; ++i) {
      std::uint64_t dummy = 0;
      iss >> dummy;
    }

    // tx: bytes packets errs drop fifo colls carrier compressed
    iss >> txBytes;

    total += (mode == NetworkBandwidthMode::Rx) ? rxBytes : txBytes;
  }

  bytesOut = total;
  return true;
}

std::optional<Sample> NetworkBandwidthCollector::sample() {
  std::uint64_t bytesTotal = 0;
  if (!readNetworkBytesTotal(mode_, ifacePrefix_, bytesTotal)) return std::nullopt;

  const auto now = std::chrono::steady_clock::now();
  if (!hasPrev_) {
    hasPrev_ = true;
    prevBytes_ = bytesTotal;
    prevTime_ = now;
    return Sample{0.0, "MB/s", ""};
  }

  const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(now - prevTime_).count();
  prevTime_ = now;

  const std::uint64_t dbytes = bytesTotal - prevBytes_;
  prevBytes_ = bytesTotal;

  if (dt <= 0.0) return Sample{0.0, "MB/s", ""};

  const double mbps = (static_cast<double>(dbytes) / (1024.0 * 1024.0)) / dt;
  return Sample{mbps, "MB/s", ifacePrefix_.empty() ? "" : ifacePrefix_};
}

}  // namespace aiz
