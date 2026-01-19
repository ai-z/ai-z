#include <aiz/dyn/cuda.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <mutex>
#include <string>

namespace aiz::dyn::cuda {
namespace {

#if defined(_WIN32)
static std::string lastErrorToString(DWORD err) {
  if (err == 0) return {};
  LPSTR buf = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD n = FormatMessageA(flags, nullptr, err, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
  std::string out;
  if (n && buf) {
    out.assign(buf, buf + n);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
  } else {
    out = "Win32Error(" + std::to_string(static_cast<unsigned long>(err)) + ")";
  }
  if (buf) LocalFree(buf);
  return out;
}
#endif

template <typename T>
static bool loadRequired(void* handle, const char* name, T& fn, std::string& err) {
  void* sym = nullptr;
#if defined(_WIN32)
  sym = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
  if (!sym) {
    err = std::string("Missing CUDA driver symbol '") + name + "': " + lastErrorToString(GetLastError());
    return false;
  }
#else
  dlerror();
  sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    err = std::string("Missing CUDA driver symbol '") + name + "': " + (e ? e : "(null)");
    return false;
  }
#endif
  fn = reinterpret_cast<T>(sym);
  return true;
}

template <typename T>
static void loadOptional(void* handle, const char* name, T& fn) {
#if defined(_WIN32)
  void* sym = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
  if (!sym) {
    fn = nullptr;
    return;
  }
#else
  dlerror();
  void* sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    fn = nullptr;
    return;
  }
#endif
  fn = reinterpret_cast<T>(sym);
}

using cuGetErrorString_t = CUresult (*)(CUresult, const char**);
static cuGetErrorString_t g_cuGetErrorString = nullptr;

static const char* kCandidates[] = {
#if defined(_WIN32)
  "nvcuda.dll",
#else
    "libcuda.so.1",
    "libcuda.so",
#endif
};

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;

static void initOnce() {
  void* handle = nullptr;
  for (const char* cand : kCandidates) {
#if defined(_WIN32)
    handle = reinterpret_cast<void*>(LoadLibraryA(cand));
#else
    handle = dlopen(cand, RTLD_LAZY | RTLD_LOCAL);
#endif
    if (handle) break;
  }

  if (!handle) {
#if defined(_WIN32)
    g_err = std::string("CUDA driver runtime not found (LoadLibrary nvcuda.dll failed): ") + lastErrorToString(GetLastError());
#else
    const char* e = dlerror();
    g_err = std::string("CUDA driver runtime not found (dlopen libcuda.so failed): ") + (e ? e : "(null)");
#endif
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
#if defined(_WIN32)
    (void)FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    g_ok = false;
    return;
  }

  loadOptional(handle, "cuGetErrorString", g_cuGetErrorString);

  // Best-effort init probe.
  const CUresult r = api.cuInit(0);
  if (r != CUDA_SUCCESS) {
    g_err = "cuInit failed: " + errToString(r);
#if defined(_WIN32)
    (void)FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
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
