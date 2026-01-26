#include <aiz/dyn/cuda.h>

#include <aiz/platform/dynlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aiz::dyn::cuda {
namespace {

template <typename T>
static bool loadRequired(platform::DynamicLibrary& lib, const char* name, T& fn, std::string& err) {
  if (!lib.loadSymbol(name, fn)) {
    err = std::string("Missing CUDA driver symbol '") + name + "'";
    return false;
  }
  return true;
}

template <typename T>
static void loadOptional(platform::DynamicLibrary& lib, const char* name, T& fn) {
  if (!lib.loadSymbol(name, fn)) {
    fn = nullptr;
  }
}

using cuGetErrorString_t = CUresult (*)(CUresult, const char**);
static cuGetErrorString_t g_cuGetErrorString = nullptr;

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;
static std::unique_ptr<platform::DynamicLibrary> g_lib;

static void initOnce() {
  std::vector<const char*> candidates;
  candidates.push_back(platform::cudaLibraryName());
#if defined(AI_Z_PLATFORM_LINUX)
  candidates.push_back("libcuda.so");
#endif

  g_lib = platform::loadLibrary(candidates, &g_err);
  if (!g_lib || !g_lib->isValid()) {
    if (g_err.empty()) g_err = "CUDA driver runtime not found";
    g_ok = false;
    return;
  }

  Api api;
  api.handle = g_lib.get();

  if (!loadRequired(*g_lib, "cuInit", api.cuInit, g_err) ||
      !loadRequired(*g_lib, "cuDriverGetVersion", api.cuDriverGetVersion, g_err) ||
      !loadRequired(*g_lib, "cuDeviceGetCount", api.cuDeviceGetCount, g_err) ||
      !loadRequired(*g_lib, "cuDeviceGet", api.cuDeviceGet, g_err) ||
      !loadRequired(*g_lib, "cuDeviceGetName", api.cuDeviceGetName, g_err) ||
      !loadRequired(*g_lib, "cuCtxCreate_v2", api.cuCtxCreate_v2, g_err) ||
      !loadRequired(*g_lib, "cuCtxDestroy_v2", api.cuCtxDestroy_v2, g_err) ||
      !loadRequired(*g_lib, "cuStreamCreate", api.cuStreamCreate, g_err) ||
      !loadRequired(*g_lib, "cuStreamDestroy_v2", api.cuStreamDestroy_v2, g_err) ||
      !loadRequired(*g_lib, "cuStreamSynchronize", api.cuStreamSynchronize, g_err) ||
      !loadRequired(*g_lib, "cuMemAlloc_v2", api.cuMemAlloc_v2, g_err) ||
      !loadRequired(*g_lib, "cuMemFree_v2", api.cuMemFree_v2, g_err) ||
      !loadRequired(*g_lib, "cuMemHostAlloc", api.cuMemHostAlloc, g_err) ||
      !loadRequired(*g_lib, "cuMemFreeHost", api.cuMemFreeHost, g_err) ||
      !loadRequired(*g_lib, "cuMemcpyHtoDAsync_v2", api.cuMemcpyHtoDAsync_v2, g_err) ||
      !loadRequired(*g_lib, "cuMemcpyDtoHAsync_v2", api.cuMemcpyDtoHAsync_v2, g_err) ||
      !loadRequired(*g_lib, "cuEventCreate", api.cuEventCreate, g_err) ||
      !loadRequired(*g_lib, "cuEventDestroy_v2", api.cuEventDestroy_v2, g_err) ||
      !loadRequired(*g_lib, "cuEventRecord", api.cuEventRecord, g_err) ||
      !loadRequired(*g_lib, "cuEventSynchronize", api.cuEventSynchronize, g_err) ||
      !loadRequired(*g_lib, "cuEventElapsedTime", api.cuEventElapsedTime, g_err) ||
      !loadRequired(*g_lib, "cuModuleLoadDataEx", api.cuModuleLoadDataEx, g_err) ||
      !loadRequired(*g_lib, "cuModuleUnload", api.cuModuleUnload, g_err) ||
      !loadRequired(*g_lib, "cuModuleGetFunction", api.cuModuleGetFunction, g_err) ||
      !loadRequired(*g_lib, "cuLaunchKernel", api.cuLaunchKernel, g_err)) {
    g_ok = false;
    return;
  }

  loadOptional(*g_lib, "cuGetErrorString", g_cuGetErrorString);

  // Best-effort init probe.
  const CUresult r = api.cuInit(0);
  if (r != CUDA_SUCCESS) {
    g_err = "cuInit failed: " + errToString(r);
    g_ok = false;
    return;
  }

  g_api = api;
  g_ok = true;
}

}  // namespace

const Api* api(std::string* errOut) {
  std::call_once(g_once, initOnce);
  if (!g_ok) {
    if (errOut) *errOut = g_err;
    return nullptr;
  }
  return &g_api;
}

std::string errToString(CUresult r) {
  if (g_cuGetErrorString) {
    const char* s = nullptr;
    if (g_cuGetErrorString(r, &s) == CUDA_SUCCESS && s) {
      return std::string{s};
    }
  }
  return "CUDA_ERROR(" + std::to_string(r) + ")";
}

}  // namespace aiz::dyn::cuda
