#include <aiz/dyn/onnxruntime.h>

#include <aiz/platform/dynlib.h>

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ORT C API minimal declarations (to avoid requiring headers at build time)
extern "C" {
  typedef struct OrtApi OrtApi;
  typedef struct OrtApiBase {
    const OrtApi* (*GetApi)(uint32_t version);
    const char* (*GetVersionString)(void);
  } OrtApiBase;

  const OrtApiBase* OrtGetApiBase(void);
}

namespace aiz {
namespace dyn {
namespace onnxruntime {

namespace {

// We don't include ORT headers here (to keep build deps light), so we can't use
// the ORT_API_VERSION macro. Instead, try a few recent versions and fall back.
//
// ORT guarantees the API is backwards-compatible; requesting an older version
// should succeed on newer runtimes.
constexpr uint32_t kOrtApiVersionsToTry[] = {22, 21, 20, 19, 18};

std::vector<std::string> getOrtSearchPaths() {
  std::vector<std::string> paths;

  // 1. Check environment variable first (user override)
  const char* custom_path = std::getenv("AI_Z_ONNXRUNTIME_PATH");
  if (custom_path) {
    paths.push_back(custom_path);
  }

#if defined(AI_Z_PLATFORM_WINDOWS)
  paths.push_back(platform::onnxruntimeLibraryName());
  return paths;
#else
  // 2. System library paths (via LD_LIBRARY_PATH or ldconfig)
  paths.push_back("libonnxruntime.so");
  paths.push_back("libonnxruntime.so.1");
  paths.push_back("libonnxruntime.so.1.18");
  paths.push_back("/usr/lib/libonnxruntime.so");
  paths.push_back("/usr/lib/x86_64-linux-gnu/libonnxruntime.so");
  paths.push_back("/usr/local/lib/libonnxruntime.so");
  paths.push_back("/opt/onnxruntime/lib/libonnxruntime.so");

  // 3. Python site-packages (common pip install location)
  // Try to find it automatically via python3
  FILE* fp = popen("python3 -c \"import onnxruntime; import os; print(os.path.join(os.path.dirname(onnxruntime.__file__), 'capi', 'libonnxruntime.so'))\" 2>/dev/null", "r");
  if (fp) {
    char buffer[512];
    if (fgets(buffer, sizeof(buffer), fp)) {
      size_t len = std::strlen(buffer);
      if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
      }
      if (std::strlen(buffer) > 0) {
        paths.push_back(buffer);
      }
    }
    pclose(fp);
  }

  // 4. Common pip install paths (fallback if python call fails)
  const char* home = std::getenv("HOME");
  if (home) {
    // User-local pip installs
    for (int minor = 8; minor <= 13; minor++) {
      std::string py_path = std::string(home) + "/.local/lib/python3." +
        std::to_string(minor) + "/site-packages/onnxruntime/capi/libonnxruntime.so";
      paths.push_back(py_path);
    }
  }

  // 5. System-wide pip install locations
  for (int minor = 8; minor <= 13; minor++) {
    std::string sys_path = "/usr/local/lib/python3." + std::to_string(minor) +
      "/dist-packages/onnxruntime/capi/libonnxruntime.so";
    paths.push_back(sys_path);

    sys_path = "/usr/lib/python3." + std::to_string(minor) +
      "/site-packages/onnxruntime/capi/libonnxruntime.so";
    paths.push_back(sys_path);
  }

  return paths;
#endif
}

struct OrtLoader {
  std::unique_ptr<platform::DynamicLibrary> handle;
  const OrtApi* api_ptr = nullptr;
  bool cuda_available = false;

  bool load(std::string* err) {
    if (api_ptr) return true;

    auto paths = getOrtSearchPaths();

    std::vector<const char*> candidates;
    candidates.reserve(paths.size());
    for (const auto& p : paths) candidates.push_back(p.c_str());

    std::string loadErr;
    handle = platform::loadLibrary(candidates, &loadErr);
    if (!handle || !handle->isValid()) {
      if (err) {
        *err = loadErr.empty() ? "ONNX Runtime not found. Install with: pip install onnxruntime" : loadErr;
      }
      return false;
    }

    // Load the OrtGetApiBase function
    using GetApiBaseFn = const OrtApiBase* (*)(void);
    auto get_api_base = reinterpret_cast<GetApiBaseFn>(handle->getSymbol("OrtGetApiBase"));

    if (!get_api_base) {
      if (err) {
        *err = "Failed to find OrtGetApiBase in libonnxruntime.";
      }
      handle.reset();
      return false;
    }

    const OrtApiBase* api_base = get_api_base();
    if (!api_base || !api_base->GetApi) {
      if (err) {
        *err = "Invalid OrtApiBase returned.";
      }
      handle.reset();
      return false;
    }

    api_ptr = nullptr;
    for (uint32_t version : kOrtApiVersionsToTry) {
      api_ptr = api_base->GetApi(version);
      if (api_ptr) {
        break;
      }
    }
    if (!api_ptr) {
      if (err) {
        *err = "Failed to get a compatible ORT API version.";
      }
      handle.reset();
      return false;
    }

    // Check for CUDA provider availability
    // Try to find the CUDA provider registration function
    cuda_available = (handle->getSymbol("OrtSessionOptionsAppendExecutionProvider_CUDA") != nullptr);

    return true;
  }
};

OrtLoader& getLoader() {
  static OrtLoader loader;
  return loader;
}

}  // namespace

const OrtApi* api(std::string* err) {
  auto& loader = getLoader();
  if (!loader.load(err)) {
    return nullptr;
  }
  return loader.api_ptr;
}

bool hasCudaProvider(std::string* err) {
  auto& loader = getLoader();
  if (!loader.load(err)) {
    return false;
  }
  return loader.cuda_available;
}

}  // namespace onnxruntime
}  // namespace dyn
}  // namespace aiz
