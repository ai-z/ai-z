#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz::platform {

struct NetworkCounters {
  std::uint64_t rxBytes = 0;
  std::uint64_t txBytes = 0;
};

std::optional<NetworkCounters> readNetworkCounters(const std::string& interfaceFilter = "");

}  // namespace aiz::platform
