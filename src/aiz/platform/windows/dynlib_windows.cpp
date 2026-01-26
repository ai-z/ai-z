#include <aiz/platform/dynlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace aiz::platform {

class WindowsDynamicLibrary final : public DynamicLibrary {
public:
  explicit WindowsDynamicLibrary(HMODULE handle) : handle_(handle) {}
  ~WindowsDynamicLibrary() override {
    if (handle_) FreeLibrary(handle_);
  }

  void* getSymbol(const char* name) override {
    return reinterpret_cast<void*>(GetProcAddress(handle_, name));
  }

  bool isValid() const override { return handle_ != nullptr; }

private:
  HMODULE handle_ = nullptr;
};

std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut) {
  for (const char* name : candidates) {
    HMODULE handle = LoadLibraryA(name);
    if (handle) return std::make_unique<WindowsDynamicLibrary>(handle);
  }

  if (errorOut) {
    DWORD err = GetLastError();
    char buf[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, static_cast<DWORD>(sizeof(buf)), nullptr);
    *errorOut = buf;
  }
  return nullptr;
}

const char* cudaLibraryName() { return "nvcuda.dll"; }
const char* nvmlLibraryName() { return "nvml.dll"; }
const char* openclLibraryName() { return "OpenCL.dll"; }
const char* vulkanLibraryName() { return "vulkan-1.dll"; }
const char* onnxruntimeLibraryName() { return "onnxruntime.dll"; }

}  // namespace aiz::platform
