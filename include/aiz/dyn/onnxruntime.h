#pragma once

#include <string>

// Forward declarations of ORT C API types
struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtMemoryInfo;
struct OrtValue;

namespace aiz {
namespace dyn {
namespace onnxruntime {

// Dynamic loader for ONNX Runtime C API.
// Returns nullptr if libonnxruntime is not found or cannot be loaded.
// If err is provided, it's populated on failure.
const OrtApi* api(std::string* err = nullptr);

// Check if CUDA execution provider is available
bool hasCudaProvider(std::string* err = nullptr);

}  // namespace onnxruntime
}  // namespace dyn
}  // namespace aiz
