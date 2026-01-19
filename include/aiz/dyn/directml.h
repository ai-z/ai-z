#pragma once

#include <string>

// Forward declarations for DirectML types
struct IDMLDevice;
struct ID3D12Device;

namespace aiz {
namespace dyn {
namespace directml {

// Dynamic loader for DirectML (Microsoft's DirectX Machine Learning).
// Returns nullptr if DirectML or D3D12 is not found or cannot be loaded.
// If err is provided, it's populated on failure.
bool isAvailable(std::string* err = nullptr);

// Check if we can create a DirectML device
bool canCreateDevice(std::string* err = nullptr);

}  // namespace directml
}  // namespace dyn
}  // namespace aiz
