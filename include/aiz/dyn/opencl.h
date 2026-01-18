#pragma once

#include <string>

#ifdef AI_Z_ENABLE_OPENCL
#include <CL/cl.h>
#endif

namespace aiz::dyn::opencl {

#ifdef AI_Z_ENABLE_OPENCL

struct Api {
  void* handle = nullptr;

  decltype(&::clGetPlatformIDs) clGetPlatformIDs = nullptr;
  decltype(&::clGetDeviceIDs) clGetDeviceIDs = nullptr;

  decltype(&::clCreateContext) clCreateContext = nullptr;
  decltype(&::clReleaseContext) clReleaseContext = nullptr;

#if defined(CL_VERSION_2_0)
  decltype(&::clCreateCommandQueueWithProperties) clCreateCommandQueueWithProperties = nullptr;
#endif
  decltype(&::clCreateCommandQueue) clCreateCommandQueue = nullptr;
  decltype(&::clReleaseCommandQueue) clReleaseCommandQueue = nullptr;

  decltype(&::clCreateBuffer) clCreateBuffer = nullptr;
  decltype(&::clReleaseMemObject) clReleaseMemObject = nullptr;

  decltype(&::clEnqueueWriteBuffer) clEnqueueWriteBuffer = nullptr;
  decltype(&::clEnqueueReadBuffer) clEnqueueReadBuffer = nullptr;

  decltype(&::clWaitForEvents) clWaitForEvents = nullptr;
  decltype(&::clReleaseEvent) clReleaseEvent = nullptr;
  decltype(&::clGetEventProfilingInfo) clGetEventProfilingInfo = nullptr;

  decltype(&::clFinish) clFinish = nullptr;

  decltype(&::clCreateProgramWithSource) clCreateProgramWithSource = nullptr;
  decltype(&::clBuildProgram) clBuildProgram = nullptr;
  decltype(&::clGetProgramBuildInfo) clGetProgramBuildInfo = nullptr;
  decltype(&::clReleaseProgram) clReleaseProgram = nullptr;

  decltype(&::clCreateKernel) clCreateKernel = nullptr;
  decltype(&::clReleaseKernel) clReleaseKernel = nullptr;
  decltype(&::clSetKernelArg) clSetKernelArg = nullptr;
  decltype(&::clEnqueueNDRangeKernel) clEnqueueNDRangeKernel = nullptr;
};

// Returns a loaded API table or nullptr if unavailable.
// When nullptr, 'errOut' (if non-null) is populated with a human-readable reason.
const Api* api(std::string* errOut = nullptr);

#endif  // AI_Z_ENABLE_OPENCL

}  // namespace aiz::dyn::opencl
