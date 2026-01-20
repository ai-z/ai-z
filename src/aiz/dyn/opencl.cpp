#include <aiz/dyn/opencl.h>

#ifdef AI_Z_ENABLE_OPENCL

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace aiz::dyn::opencl {
namespace {

template <typename T>
static bool loadRequired(void* handle, const char* name, T& fn, std::string& err) {
  dlerror();
  void* sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    err = std::string("Missing OpenCL symbol '") + name + "': " + (e ? e : "(null)");
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

static const char* kCandidates[] = {
  "libOpenCL.so.1",
  "libOpenCL.so",
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
    g_err = std::string("OpenCL runtime not found (dlopen libOpenCL.so failed): ") + (e ? e : "(null)");
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
    dlclose(handle);
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
