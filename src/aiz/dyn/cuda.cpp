#include <aiz/dyn/cuda.h>

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace aiz::dyn::cuda {
namespace {

template <typename T>
static bool loadRequired(void* handle, const char* name, T& fn, std::string& err) {
  dlerror();
  void* sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    err = std::string("Missing CUDA driver symbol '") + name + "': " + (e ? e : "(null)");
    return false;
  }
  fn = reinterpret_cast<T>(sym);
  return true;
}

template <typename T>
static void loadOptional(void* handle, const char* name, T& fn) {
  dlerror();
  void* sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    fn = nullptr;
    return;
  }
  fn = reinterpret_cast<T>(sym);
}

using cuGetErrorString_t = CUresult (*)(CUresult, const char**);
static cuGetErrorString_t g_cuGetErrorString = nullptr;

static const char* kCandidates[] = {
    "libcuda.so.1",
    "libcuda.so",
};

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;

static void initOnce() {
  void* handle = nullptr;
  for (const char* cand : kCandidates) {
    handle = dlopen(cand, RTLD_LAZY | RTLD_LOCAL);
    if (handle) break;
  }

  if (!handle) {
    const char* e = dlerror();
    g_err = std::string("CUDA driver runtime not found (dlopen libcuda.so failed): ") + (e ? e : "(null)");
    g_ok = false;
    return;
  }

  Api api;
  api.handle = handle;

  if (!loadRequired(handle, "cuInit", api.cuInit, g_err) ||
      !loadRequired(handle, "cuDriverGetVersion", api.cuDriverGetVersion, g_err) ||
      !loadRequired(handle, "cuDeviceGetCount", api.cuDeviceGetCount, g_err) ||
      !loadRequired(handle, "cuDeviceGet", api.cuDeviceGet, g_err) ||
      !loadRequired(handle, "cuDeviceGetName", api.cuDeviceGetName, g_err) ||
      !loadRequired(handle, "cuCtxCreate_v2", api.cuCtxCreate_v2, g_err) ||
      !loadRequired(handle, "cuCtxDestroy_v2", api.cuCtxDestroy_v2, g_err) ||
      !loadRequired(handle, "cuStreamCreate", api.cuStreamCreate, g_err) ||
      !loadRequired(handle, "cuStreamDestroy_v2", api.cuStreamDestroy_v2, g_err) ||
      !loadRequired(handle, "cuStreamSynchronize", api.cuStreamSynchronize, g_err) ||
      !loadRequired(handle, "cuMemAlloc_v2", api.cuMemAlloc_v2, g_err) ||
      !loadRequired(handle, "cuMemFree_v2", api.cuMemFree_v2, g_err) ||
      !loadRequired(handle, "cuMemHostAlloc", api.cuMemHostAlloc, g_err) ||
      !loadRequired(handle, "cuMemFreeHost", api.cuMemFreeHost, g_err) ||
      !loadRequired(handle, "cuMemcpyHtoDAsync_v2", api.cuMemcpyHtoDAsync_v2, g_err) ||
      !loadRequired(handle, "cuMemcpyDtoHAsync_v2", api.cuMemcpyDtoHAsync_v2, g_err) ||
      !loadRequired(handle, "cuEventCreate", api.cuEventCreate, g_err) ||
      !loadRequired(handle, "cuEventDestroy_v2", api.cuEventDestroy_v2, g_err) ||
      !loadRequired(handle, "cuEventRecord", api.cuEventRecord, g_err) ||
      !loadRequired(handle, "cuEventSynchronize", api.cuEventSynchronize, g_err) ||
      !loadRequired(handle, "cuEventElapsedTime", api.cuEventElapsedTime, g_err) ||
      !loadRequired(handle, "cuModuleLoadDataEx", api.cuModuleLoadDataEx, g_err) ||
      !loadRequired(handle, "cuModuleUnload", api.cuModuleUnload, g_err) ||
      !loadRequired(handle, "cuModuleGetFunction", api.cuModuleGetFunction, g_err) ||
      !loadRequired(handle, "cuLaunchKernel", api.cuLaunchKernel, g_err)) {
    dlclose(handle);
    g_ok = false;
    return;
  }

  loadOptional(handle, "cuGetErrorString", g_cuGetErrorString);

  // Best-effort init probe.
  const CUresult r = api.cuInit(0);
  if (r != CUDA_SUCCESS) {
    g_err = "cuInit failed: " + errToString(r);
    dlclose(handle);
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
