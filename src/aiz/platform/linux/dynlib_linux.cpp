#include <aiz/platform/dynlib.h>

#include <dlfcn.h>

namespace aiz::platform {

class LinuxDynamicLibrary final : public DynamicLibrary {
public:
  explicit LinuxDynamicLibrary(void* handle) : handle_(handle) {}
  ~LinuxDynamicLibrary() override {
    if (handle_) dlclose(handle_);
  }

  void* getSymbol(const char* name) override {
    dlerror();
    return dlsym(handle_, name);
  }

  bool isValid() const override { return handle_ != nullptr; }

private:
  void* handle_ = nullptr;
};

std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut) {
  for (const char* name : candidates) {
    void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    if (handle) return std::make_unique<LinuxDynamicLibrary>(handle);
  }

  if (errorOut) {
    const char* e = dlerror();
    *errorOut = e ? e : "Library not found";
  }
  return nullptr;
}

const char* cudaLibraryName() { return "libcuda.so.1"; }
const char* nvmlLibraryName() { return "libnvidia-ml.so.1"; }
const char* openclLibraryName() { return "libOpenCL.so.1"; }
const char* vulkanLibraryName() { return "libvulkan.so.1"; }
const char* onnxruntimeLibraryName() { return "libonnxruntime.so"; }

}  // namespace aiz::platform
