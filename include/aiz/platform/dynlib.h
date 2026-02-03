#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aiz::platform {

class DynamicLibrary {
public:
  virtual ~DynamicLibrary() = default;
  virtual void* getSymbol(const char* name) = 0;
  virtual bool isValid() const = 0;

  template <typename T>
  bool loadSymbol(const char* name, T& fn) {
    fn = reinterpret_cast<T>(getSymbol(name));
    return fn != nullptr;
  }

  template <typename T>
  bool loadRequired(const char* name, T& fn, std::string& err) {
    if (!loadSymbol(name, fn)) {
      err = std::string("Missing symbol: ") + name;
      return false;
    }
    return true;
  }
};

std::unique_ptr<DynamicLibrary> loadLibrary(
    const std::vector<const char*>& candidates,
    std::string* errorOut = nullptr);

const char* cudaLibraryName();
const char* nvmlLibraryName();
const char* openclLibraryName();
const char* vulkanLibraryName();
const char* onnxruntimeLibraryName();

}  // namespace aiz::platform
