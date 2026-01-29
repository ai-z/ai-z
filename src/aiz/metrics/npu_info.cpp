#include <aiz/metrics/npu_info.h>
#include <aiz/dyn/onnxruntime.h>

#include <algorithm>
#include <sstream>
#include <string>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace aiz {

std::string npuVendorToString(NpuVendor vendor) {
  switch (vendor) {
    case NpuVendor::Intel: return "Intel";
    case NpuVendor::AMD: return "AMD";
    default: return "Unknown";
  }
}

std::string npuStatusToString(NpuStatus status) {
  switch (status) {
    case NpuStatus::Available: return "Available";
    case NpuStatus::NoDevice: return "No device";
    case NpuStatus::NoDriver: return "No driver";
    case NpuStatus::Unsupported: return "Unsupported";
    default: return "Unknown";
  }
}

NpuAvailability probeNpuDevices() {
  NpuAvailability result;
  result.status = NpuStatus::NoDevice;
  
  std::ostringstream diag;
  diag << "NPU Probe Results:\n";
  
  // Probe Intel NPU
  NpuAvailability intelResult = detail::probeIntelNpu();
  if (intelResult.status == NpuStatus::Available) {
    for (auto& dev : intelResult.devices) {
      result.devices.push_back(std::move(dev));
    }
    result.status = NpuStatus::Available;
  } else if (intelResult.status == NpuStatus::NoDriver && result.status != NpuStatus::Available) {
    result.status = NpuStatus::NoDriver;
  }
  diag << "  Intel: " << intelResult.diagnostics << "\n";
  
  // Probe AMD NPU
  NpuAvailability amdResult = detail::probeAmdNpu();
  if (amdResult.status == NpuStatus::Available) {
    for (auto& dev : amdResult.devices) {
      result.devices.push_back(std::move(dev));
    }
    result.status = NpuStatus::Available;
  } else if (amdResult.status == NpuStatus::NoDriver && result.status != NpuStatus::Available) {
    result.status = NpuStatus::NoDriver;
  }
  diag << "  AMD: " << amdResult.diagnostics << "\n";
  
  // Set overall diagnostics
  if (result.devices.empty()) {
    if (result.status == NpuStatus::NoDriver) {
      result.diagnostics = "NPU hardware detected but driver not loaded.";
    } else {
      result.diagnostics = "No NPU devices detected.";
    }
  } else {
    std::ostringstream summary;
    summary << result.devices.size() << " NPU(s) available: ";
    for (size_t i = 0; i < result.devices.size(); ++i) {
      if (i > 0) summary << ", ";
      summary << result.devices[i].name;
    }
    result.diagnostics = summary.str();
  }
  
  return result;
}

OrtNpuProvider probeOrtNpuProviders() {
  OrtNpuProvider result;
  
  // Check if ONNX Runtime is available at all
  std::string err;
  const OrtApi* ort = dyn::onnxruntime::api(&err);
  if (!ort) {
    return result;  // ORT not available
  }
  
#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
  // Get available execution providers
  // Note: The exact provider names and availability depend on the ORT build
  
  // Intel NPU providers:
  // - "OpenVINOExecutionProvider" - Intel OpenVINO EP (supports Intel NPU via AUTO:NPU device)
  // - "NPUExecutionProvider" - Direct Intel NPU EP (newer ORT builds)
  // - "QNNExecutionProvider" - Qualcomm NPU (not Intel, but for completeness)
  
  // AMD NPU providers:
  // - "VitisAIExecutionProvider" - AMD Vitis AI EP (supports AMD XDNA NPU)
  // - "AMDNPUExecutionProvider" - Direct AMD NPU EP (potential future provider)
  
  // To properly detect available providers, we would need to:
  // 1. Create a session options object
  // 2. Try to append each execution provider
  // 3. Check if the operation succeeds
  
  // However, a simpler approach is to check for the provider DLLs/SOs
  
  // For now, we use a heuristic based on symbol availability
  // This could be enhanced with actual provider enumeration
  
#if defined(_WIN32)
  // On Windows, check for provider-specific DLLs
  {
    HMODULE hOpenVino = LoadLibraryW(L"onnxruntime_providers_openvino.dll");
    if (hOpenVino) {
      result.intelNpuAvailable = true;
      result.intelProviderName = "OpenVINOExecutionProvider";
      FreeLibrary(hOpenVino);
    }
    
    HMODULE hVitisAI = LoadLibraryW(L"onnxruntime_providers_vitisai.dll");
    if (hVitisAI) {
      result.amdNpuAvailable = true;
      result.amdProviderName = "VitisAIExecutionProvider";
      FreeLibrary(hVitisAI);
    }
  }
#else
  // On Linux, check for provider shared objects
  {
    void* hOpenVino = dlopen("libonnxruntime_providers_openvino.so", RTLD_NOW | RTLD_LOCAL);
    if (hOpenVino) {
      result.intelNpuAvailable = true;
      result.intelProviderName = "OpenVINOExecutionProvider";
      dlclose(hOpenVino);
    }
    
    void* hVitisAI = dlopen("libonnxruntime_providers_vitisai.so", RTLD_NOW | RTLD_LOCAL);
    if (hVitisAI) {
      result.amdNpuAvailable = true;
      result.amdProviderName = "VitisAIExecutionProvider";
      dlclose(hVitisAI);
    }
  }
#endif
  
#endif  // AI_Z_HAS_ONNXRUNTIME_C_API
  
  return result;
}

// Stub implementations for platforms where the specific probe isn't available
#if !defined(__linux__) && !defined(_WIN32)

namespace detail {

NpuAvailability probeIntelNpu() {
  NpuAvailability result;
  result.status = NpuStatus::Unsupported;
  result.diagnostics = "Intel NPU detection not supported on this platform.";
  return result;
}

NpuAvailability probeAmdNpu() {
  NpuAvailability result;
  result.status = NpuStatus::Unsupported;
  result.diagnostics = "AMD NPU detection not supported on this platform.";
  return result;
}

}  // namespace detail

#endif

}  // namespace aiz
