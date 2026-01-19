#include <aiz/dyn/directml.h>

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace aiz {
namespace dyn {
namespace directml {

namespace {

std::vector<std::string> getDirectMLSearchPaths() {
  std::vector<std::string> paths;

  // 1. Check environment variable first (user override)
  const char* custom_path = std::getenv("AI_Z_DIRECTML_PATH");
  if (custom_path) {
    paths.push_back(custom_path);
  }

  // 2. System library paths
  paths.push_back("libdirectml.so");
  paths.push_back("libdirectml.so.1");
  paths.push_back("/usr/lib/libdirectml.so");
  paths.push_back("/usr/lib/x86_64-linux-gnu/libdirectml.so");
  paths.push_back("/usr/local/lib/libdirectml.so");

  // 3. Python site-packages (pip install onnxruntime-directml)
  FILE* fp = popen("python3 -c \"import onnxruntime; import os; print(os.path.join(os.path.dirname(onnxruntime.__file__), 'capi', 'libdirectml.so'))\" 2>/dev/null", "r");
  if (fp) {
    char buffer[512];
    if (fgets(buffer, sizeof(buffer), fp)) {
      size_t len = std::strlen(buffer);
      if (len > 0 && buffer[len-1] == '\n') {
        buffer[len-1] = '\0';
      }
      if (std::strlen(buffer) > 0) {
        paths.push_back(buffer);
      }
    }
    pclose(fp);
  }

  // 4. Common pip install paths
  const char* home = std::getenv("HOME");
  if (home) {
    for (int minor = 8; minor <= 13; minor++) {
      std::string py_path = std::string(home) + "/.local/lib/python3." +
        std::to_string(minor) + "/site-packages/onnxruntime/capi/libdirectml.so";
      paths.push_back(py_path);
    }
  }

  return paths;
}

struct DirectMLLoader {
  void* dml_handle = nullptr;
  void* d3d12_handle = nullptr;

  ~DirectMLLoader() {
    if (dml_handle) dlclose(dml_handle);
    if (d3d12_handle) dlclose(d3d12_handle);
  }

  bool load(std::string* err) {
    if (dml_handle) return true;

    // DirectML requires D3D12 on Linux (via Wine/Proton or native Mesa implementation)
    // Try to load D3D12 first
    const char* d3d12_names[] = {"libd3d12.so", "/usr/lib/x86_64-linux-gnu/libd3d12.so", nullptr};
    for (int i = 0; d3d12_names[i]; i++) {
      d3d12_handle = dlopen(d3d12_names[i], RTLD_LAZY | RTLD_LOCAL);
      if (d3d12_handle) break;
    }

    // DirectML might work without D3D12 on some systems, so continue

    auto paths = getDirectMLSearchPaths();
    for (const auto& path : paths) {
      dml_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
      if (dml_handle) break;
    }

    if (!dml_handle) {
      if (err) {
        *err = "DirectML not found. Install with: pip install onnxruntime-directml";
      }
      return false;
    }

    return true;
  }
};

DirectMLLoader& getLoader() {
  static DirectMLLoader loader;
  return loader;
}

}  // namespace

bool isAvailable(std::string* err) {
  auto& loader = getLoader();
  return loader.load(err);
}

bool canCreateDevice(std::string* err) {
  // For now, just check if the library loads
  // A full implementation would try to create a D3D12 device and DML device
  return isAvailable(err);
}

}  // namespace directml
}  // namespace dyn
}  // namespace aiz
