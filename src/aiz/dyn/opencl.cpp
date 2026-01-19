#include <aiz/dyn/opencl.h>

#ifdef AI_Z_ENABLE_OPENCL

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <mutex>
#include <string>

namespace aiz::dyn::opencl {
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
    err = std::string("Missing OpenCL symbol '") + name + "': " + lastErrorToString(GetLastError());
    return false;
  }
#else
  dlerror();
  sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    err = std::string("Missing OpenCL symbol '") + name + "': " + (e ? e : "(null)");
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

static const char* kCandidates[] = {
#if defined(_WIN32)
  "OpenCL.dll",
#else
    "libOpenCL.so.1",
    "libOpenCL.so",
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
    g_err = std::string("OpenCL runtime not found (LoadLibrary OpenCL.dll failed): ") + lastErrorToString(GetLastError());
#else
    const char* e = dlerror();
    g_err = std::string("OpenCL runtime not found (dlopen libOpenCL.so failed): ") + (e ? e : "(null)");
#endif
    g_ok = false;
    return;
  }

  Api api;
  api.handle = handle;

  // Required core symbols.
  if (!loadRequired(handle, "clGetPlatformIDs", api.clGetPlatformIDs, g_err) ||
      !loadRequired(handle, "clGetDeviceIDs", api.clGetDeviceIDs, g_err) ||
      !loadRequired(handle, "clCreateContext", api.clCreateContext, g_err) ||
      !loadRequired(handle, "clReleaseContext", api.clReleaseContext, g_err) ||
      !loadRequired(handle, "clCreateCommandQueue", api.clCreateCommandQueue, g_err) ||
      !loadRequired(handle, "clReleaseCommandQueue", api.clReleaseCommandQueue, g_err) ||
      !loadRequired(handle, "clCreateBuffer", api.clCreateBuffer, g_err) ||
      !loadRequired(handle, "clReleaseMemObject", api.clReleaseMemObject, g_err) ||
      !loadRequired(handle, "clEnqueueWriteBuffer", api.clEnqueueWriteBuffer, g_err) ||
      !loadRequired(handle, "clEnqueueReadBuffer", api.clEnqueueReadBuffer, g_err) ||
      !loadRequired(handle, "clWaitForEvents", api.clWaitForEvents, g_err) ||
      !loadRequired(handle, "clReleaseEvent", api.clReleaseEvent, g_err) ||
      !loadRequired(handle, "clGetEventProfilingInfo", api.clGetEventProfilingInfo, g_err) ||
      !loadRequired(handle, "clFinish", api.clFinish, g_err) ||
      !loadRequired(handle, "clCreateProgramWithSource", api.clCreateProgramWithSource, g_err) ||
      !loadRequired(handle, "clBuildProgram", api.clBuildProgram, g_err) ||
      !loadRequired(handle, "clGetProgramBuildInfo", api.clGetProgramBuildInfo, g_err) ||
      !loadRequired(handle, "clReleaseProgram", api.clReleaseProgram, g_err) ||
      !loadRequired(handle, "clCreateKernel", api.clCreateKernel, g_err) ||
      !loadRequired(handle, "clReleaseKernel", api.clReleaseKernel, g_err) ||
      !loadRequired(handle, "clSetKernelArg", api.clSetKernelArg, g_err) ||
      !loadRequired(handle, "clEnqueueNDRangeKernel", api.clEnqueueNDRangeKernel, g_err)) {
#if defined(_WIN32)
    (void)FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    g_ok = false;
    return;
  }

#if defined(CL_VERSION_2_0)
  loadOptional(handle, "clCreateCommandQueueWithProperties", api.clCreateCommandQueueWithProperties);
#endif

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

}  // namespace aiz::dyn::opencl

#endif  // AI_Z_ENABLE_OPENCL
