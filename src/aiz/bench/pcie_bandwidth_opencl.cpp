#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_OPENCL

#include <aiz/dyn/opencl.h>

#include <CL/cl.h>

#include <chrono>
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
    case CL_PROFILING_INFO_NOT_AVAILABLE: return "CL_PROFILING_INFO_NOT_AVAILABLE";
    case CL_MEM_COPY_OVERLAP: return "CL_MEM_COPY_OVERLAP";
    case CL_IMAGE_FORMAT_MISMATCH: return "CL_IMAGE_FORMAT_MISMATCH";
    case CL_IMAGE_FORMAT_NOT_SUPPORTED: return "CL_IMAGE_FORMAT_NOT_SUPPORTED";
    case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
    case CL_INVALID_DEVICE: return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT: return "CL_INVALID_CONTEXT";
    case CL_INVALID_MEM_OBJECT: return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_COMMAND_QUEUE: return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_EVENT: return "CL_INVALID_EVENT";
    case CL_INVALID_OPERATION: return "CL_INVALID_OPERATION";
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

class OpenClPcieBandwidth final : public IBenchmark {
public:
  explicit OpenClPcieBandwidth(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

  std::string name() const override { return "OpenCL PCIe bandwidth"; }

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

    constexpr std::size_t bytes = 256ull * 1024ull * 1024ull;
    constexpr int warmup = 2;
    constexpr int iters = 10;

    // Host pinned memory isn't guaranteed in OpenCL across ICDs; use ordinary host memory.
    std::vector<std::uint8_t> host(bytes);
    std::memset(host.data(), 0xA5, host.size());

    cl_mem buf = cl->clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &e);
    if (!buf || e != CL_SUCCESS) {
      cl->clReleaseCommandQueue(q);
      cl->clReleaseContext(ctx);
      return BenchResult{false, "clCreateBuffer failed: " + clErrToString(e)};
    }

    auto measure = [&](bool h2d, double& gbpsOut) -> bool {
      // Warmup.
      for (int i = 0; i < warmup; ++i) {
        if (h2d) {
          e = cl->clEnqueueWriteBuffer(q, buf, CL_TRUE, 0, bytes, host.data(), 0, nullptr, nullptr);
        } else {
          e = cl->clEnqueueReadBuffer(q, buf, CL_TRUE, 0, bytes, host.data(), 0, nullptr, nullptr);
        }
        if (e != CL_SUCCESS) {
          err = std::string("enqueue warmup failed: ") + clErrToString(e);
          return false;
        }
      }

      // Timed: average over iters.
      double totalSec = 0.0;
      for (int i = 0; i < iters; ++i) {
        cl_event ev{};
        if (h2d) {
          e = cl->clEnqueueWriteBuffer(q, buf, CL_FALSE, 0, bytes, host.data(), 0, nullptr, &ev);
        } else {
          e = cl->clEnqueueReadBuffer(q, buf, CL_FALSE, 0, bytes, host.data(), 0, nullptr, &ev);
        }
        if (e != CL_SUCCESS || !ev) {
          err = std::string("enqueue timed failed: ") + clErrToString(e);
          return false;
        }
        e = cl->clWaitForEvents(1, &ev);
        if (e != CL_SUCCESS) {
          cl->clReleaseEvent(ev);
          err = std::string("clWaitForEvents failed: ") + clErrToString(e);
          return false;
        }
        auto sec = eventSeconds(ev, err);
        cl->clReleaseEvent(ev);
        if (!sec) return false;
        totalSec += *sec;
      }

      const double avgSec = totalSec / static_cast<double>(iters);
      if (avgSec <= 0.0) {
        err = "timing failed";
        return false;
      }
      const double gbps = (static_cast<double>(bytes) / avgSec) / 1e9;
      gbpsOut = gbps;
      return true;
    };

    double rx = 0.0;
    double tx = 0.0;
    const bool okRx = measure(true, rx);   // host->device
    const bool okTx = okRx ? measure(false, tx) : false;  // device->host

    cl->clReleaseMemObject(buf);
    cl->clReleaseCommandQueue(q);
    cl->clReleaseContext(ctx);

    if (!okRx || !okTx) return BenchResult{false, err.empty() ? std::string("transfer failed") : err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "RX: " << rx << " GB/s, TX: " << tx << " GB/s";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkOpenclBackend(unsigned int gpuIndex) {
  return std::make_unique<OpenClPcieBandwidth>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_OPENCL
