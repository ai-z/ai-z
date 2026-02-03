#pragma once

#include <filesystem>

namespace aiz::platform {

std::filesystem::path configDirectory();
std::filesystem::path dataDirectory();

}  // namespace aiz::platform
