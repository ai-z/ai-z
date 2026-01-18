#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_OPENCL

#include <aiz/dyn/opencl.h>

#include <CL/cl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace aiz {
namespace {

static std::string clErrToString(cl_int e) {
  switch (e) {
    case CL_SUCCESS: return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE: return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE: return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
    case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
    default: break;
  }
  return "CL_ERROR(" + std::to_string(static_cast<int>(e)) + ")";
}

static std::optional<std::vector<cl_device_id>> listGpuDevices(std::string& err) {
  const auto* cl = ::aiz::dyn::opencl::api(&err);
  if (!cl) return std::nullopt;

  cl_uint numPlatforms = 0;
  cl_int e = cl->clGetPlatformIDs(0, nullptr, &numPlatforms);
  if (e != CL_SUCCESS) {
    err = "clGetPlatformIDs failed: " + clErrToString(e);
    return std::nullopt;
  }
  if (numPlatforms == 0) {
    err = "No OpenCL platforms found.";
    return std::nullopt;
  }

  std::vector<cl_platform_id> platforms(numPlatforms);
  e = cl->clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);
  if (e != CL_SUCCESS) {
    err = "clGetPlatformIDs(list) failed: " + clErrToString(e);
    return std::nullopt;
  }

  std::vector<cl_device_id> devices;
  for (auto p : platforms) {
    cl_uint n = 0;
    e = cl->clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &n);
    if (e == CL_DEVICE_NOT_FOUND) continue;
    if (e != CL_SUCCESS) continue;
    if (n == 0) continue;
    std::vector<cl_device_id> ds(n);
    e = cl->clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, n, ds.data(), nullptr);
    if (e != CL_SUCCESS) continue;
    for (auto d : ds) devices.push_back(d);
  }

  if (devices.empty()) {
    err = "No OpenCL GPU devices found.";
    return std::nullopt;
  }
  return devices;
}

static std::optional<double> eventSeconds(cl_event ev, std::string& err) {
  const auto* cl = ::aiz::dyn::opencl::api(&err);
  if (!cl) return std::nullopt;

  cl_ulong start = 0;
  cl_ulong end = 0;
  cl_int e = cl->clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr);
  if (e != CL_SUCCESS) {
    err = "clGetEventProfilingInfo(START) failed: " + clErrToString(e);
    return std::nullopt;
  }
  e = cl->clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr);
  if (e != CL_SUCCESS) {
    err = "clGetEventProfilingInfo(END) failed: " + clErrToString(e);
    return std::nullopt;
  }
  if (end <= start) {
    err = "profiling timestamps invalid";
    return std::nullopt;
  }
  const double ns = static_cast<double>(end - start);
  return ns * 1e-9;
}

static const char* kKernelSrc = R"CLC(
__kernel void fma_bench(__global float* out) {
  const uint gid = get_global_id(0);
  float x = (float)gid;
  // Fixed iteration count keeps kernel simple and portable.
  // Each loop body does 2 FLOPs (mul+add).
  for (int i = 0; i < 4096; ++i) {
    x = x * 1.0000001f + 0.0000001f;
  }
  out[gid] = x;
}
)CLC";

class OpenClFp32Flops final : public IBenchmark {
public:
  explicit OpenClFp32Flops(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

  std::string name() const override { return "OpenCL FLOPS FP32"; }

  bool isAvailable() const override {
    std::string err;
    auto devs = listGpuDevices(err);
    return devs && (gpuIndex_ < devs->size());
  }

  BenchResult run() override {
    std::string err;
    const auto* cl = ::aiz::dyn::opencl::api(&err);
    if (!cl) return BenchResult{false, err};

    auto devs = listGpuDevices(err);
    if (!devs) return BenchResult{false, err};
    if (gpuIndex_ >= devs->size()) return BenchResult{false, "Invalid OpenCL GPU index."};

    cl_device_id dev = (*devs)[gpuIndex_];
    cl_int e = CL_SUCCESS;
    cl_context ctx = cl->clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &e);
    if (!ctx || e != CL_SUCCESS) return BenchResult{false, "clCreateContext failed: " + clErrToString(e)};

#if defined(CL_VERSION_2_0)
    cl_command_queue q{};
    if (cl->clCreateCommandQueueWithProperties) {
      const cl_queue_properties props[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
      q = cl->clCreateCommandQueueWithProperties(ctx, dev, props, &e);
    } else {
      q = cl->clCreateCommandQueue(ctx, dev, CL_QUEUE_PROFILING_ENABLE, &e);
    }
#else
    cl_command_queue q = cl->clCreateCommandQueue(ctx, dev, CL_QUEUE_PROFILING_ENABLE, &e);
#endif
    if (!q || e != CL_SUCCESS) {
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clCreateCommandQueue failed: " + clErrToString(e)};
    }

    const char* srcs[] = {kKernelSrc};
    const std::size_t lens[] = {std::strlen(kKernelSrc)};
    cl_program prog = cl->clCreateProgramWithSource(ctx, 1, srcs, lens, &e);
    if (!prog || e != CL_SUCCESS) {
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clCreateProgramWithSource failed: " + clErrToString(e)};
    }

    e = cl->clBuildProgram(prog, 1, &dev, "-cl-fast-relaxed-math", nullptr, nullptr);
    if (e != CL_SUCCESS) {
      // Grab build log.
      std::size_t logSize = 0;
      cl->clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);
      std::string log;
      if (logSize > 0) {
        log.resize(logSize);
        cl->clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, logSize, log.data(), nullptr);
      }
      cl->clReleaseProgram(prog);
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clBuildProgram failed: " + clErrToString(e) + (log.empty() ? std::string() : ("\n" + log))};
    }

    cl_kernel k = cl->clCreateKernel(prog, "fma_bench", &e);
    if (!k || e != CL_SUCCESS) {
      cl->clReleaseProgram(prog);
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clCreateKernel failed: " + clErrToString(e)};
    }

    constexpr std::size_t n = 1u << 20;  // 1,048,576 work-items
    cl_mem outBuf = cl->clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * sizeof(float), nullptr, &e);
    if (!outBuf || e != CL_SUCCESS) {
      cl->clReleaseKernel(k);
      cl->clReleaseProgram(prog);
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clCreateBuffer(out) failed: " + clErrToString(e)};
    }

    e = cl->clSetKernelArg(k, 0, sizeof(outBuf), &outBuf);
    if (e != CL_SUCCESS) {
      cl->clReleaseMemObject(outBuf);
      cl->clReleaseKernel(k);
      cl->clReleaseProgram(prog);
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clSetKernelArg failed: " + clErrToString(e)};
    }

    constexpr int warmup = 1;
    for (int i = 0; i < warmup; ++i) {
      const std::size_t global = n;
      e = cl->clEnqueueNDRangeKernel(q, k, 1, nullptr, &global, nullptr, 0, nullptr, nullptr);
      if (e != CL_SUCCESS) {
        cl->clReleaseMemObject(outBuf);
        cl->clReleaseKernel(k);
        cl->clReleaseProgram(prog);
        cl->clReleaseCommandQueue(q);
        cl->clReleaseContext(ctx);
        return BenchResult{false, "warmup kernel enqueue failed: " + clErrToString(e)};
      }
      cl->clFinish(q);
    }

    cl_event ev{};
    const std::size_t global = n;
    e = cl->clEnqueueNDRangeKernel(q, k, 1, nullptr, &global, nullptr, 0, nullptr, &ev);
    if (e != CL_SUCCESS || !ev) {
      cl->clReleaseMemObject(outBuf);
      cl->clReleaseKernel(k);
      cl->clReleaseProgram(prog);
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "kernel enqueue failed: " + clErrToString(e)};
    }

    cl->clWaitForEvents(1, &ev);
    auto sec = eventSeconds(ev, err);
    cl->clReleaseEvent(ev);

    cl->clReleaseMemObject(outBuf);
    cl->clReleaseKernel(k);
    cl->clReleaseProgram(prog);
    cl->clReleaseCommandQueue(q);
    cl->clReleaseContext(ctx);

    if (!sec) return BenchResult{false, err};
    if (*sec <= 0.0) return BenchResult{false, "timing failed"};

    // 4096 iterations * 2 FLOPs (mul+add) per work-item.
    const double flops = static_cast<double>(n) * 4096.0 * 2.0;
    const double gflops = (flops / *sec) / 1e9;

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(2);
    oss << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkOpenclBackend(unsigned int gpuIndex) {
  return std::make_unique<OpenClFp32Flops>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_OPENCL
