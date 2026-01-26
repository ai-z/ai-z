#include <aiz/dyn/opencl.h>

#ifdef AI_Z_ENABLE_OPENCL

#include <aiz/platform/dynlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aiz::dyn::opencl {
namespace {

template <typename T>
static bool loadRequired(platform::DynamicLibrary& lib, const char* name, T& fn, std::string& err) {
  if (!lib.loadSymbol(name, fn)) {
    err = std::string("Missing OpenCL symbol '") + name + "'";
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

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;
static std::unique_ptr<platform::DynamicLibrary> g_lib;

static void initOnce() {
  std::vector<const char*> candidates;
  candidates.push_back(platform::openclLibraryName());
#if defined(AI_Z_PLATFORM_LINUX)
  candidates.push_back("libOpenCL.so");
#endif

  g_lib = platform::loadLibrary(candidates, &g_err);
  if (!g_lib || !g_lib->isValid()) {
    if (g_err.empty()) g_err = "OpenCL runtime not found";
    g_ok = false;
    return;
  }

  Api api;
  api.handle = g_lib.get();

  // Required core symbols.
  if (!loadRequired(*g_lib, "clGetPlatformIDs", api.clGetPlatformIDs, g_err) ||
      !loadRequired(*g_lib, "clGetDeviceIDs", api.clGetDeviceIDs, g_err) ||
      !loadRequired(*g_lib, "clCreateContext", api.clCreateContext, g_err) ||
      !loadRequired(*g_lib, "clReleaseContext", api.clReleaseContext, g_err) ||
      !loadRequired(*g_lib, "clCreateCommandQueue", api.clCreateCommandQueue, g_err) ||
      !loadRequired(*g_lib, "clReleaseCommandQueue", api.clReleaseCommandQueue, g_err) ||
      !loadRequired(*g_lib, "clCreateBuffer", api.clCreateBuffer, g_err) ||
      !loadRequired(*g_lib, "clReleaseMemObject", api.clReleaseMemObject, g_err) ||
      !loadRequired(*g_lib, "clEnqueueWriteBuffer", api.clEnqueueWriteBuffer, g_err) ||
      !loadRequired(*g_lib, "clEnqueueReadBuffer", api.clEnqueueReadBuffer, g_err) ||
      !loadRequired(*g_lib, "clWaitForEvents", api.clWaitForEvents, g_err) ||
      !loadRequired(*g_lib, "clReleaseEvent", api.clReleaseEvent, g_err) ||
      !loadRequired(*g_lib, "clGetEventProfilingInfo", api.clGetEventProfilingInfo, g_err) ||
      !loadRequired(*g_lib, "clFinish", api.clFinish, g_err) ||
      !loadRequired(*g_lib, "clCreateProgramWithSource", api.clCreateProgramWithSource, g_err) ||
      !loadRequired(*g_lib, "clBuildProgram", api.clBuildProgram, g_err) ||
      !loadRequired(*g_lib, "clGetProgramBuildInfo", api.clGetProgramBuildInfo, g_err) ||
      !loadRequired(*g_lib, "clReleaseProgram", api.clReleaseProgram, g_err) ||
      !loadRequired(*g_lib, "clCreateKernel", api.clCreateKernel, g_err) ||
      !loadRequired(*g_lib, "clReleaseKernel", api.clReleaseKernel, g_err) ||
      !loadRequired(*g_lib, "clSetKernelArg", api.clSetKernelArg, g_err) ||
      !loadRequired(*g_lib, "clEnqueueNDRangeKernel", api.clEnqueueNDRangeKernel, g_err)) {
    g_ok = false;
    return;
  }

#if defined(CL_VERSION_2_0)
  loadOptional(*g_lib, "clCreateCommandQueueWithProperties", api.clCreateCommandQueueWithProperties);
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
