#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <aiz/dyn/cuda.h>

#include <cstddef>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace aiz {
namespace {

static BenchResult failCuda(const std::string& where, ::aiz::dyn::cuda::CUresult r) {
  return BenchResult{false, where + ": " + ::aiz::dyn::cuda::errToString(r)};
}

struct CudaEventPair {
  ::aiz::dyn::cuda::CUevent start{};
  ::aiz::dyn::cuda::CUevent stop{};
};

static std::optional<CudaEventPair> createEvents(const ::aiz::dyn::cuda::Api* cu, std::string& err) {
  CudaEventPair p;
  auto r = cu->cuEventCreate(&p.start, ::aiz::dyn::cuda::CU_EVENT_DEFAULT);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuEventCreate(start) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }
  r = cu->cuEventCreate(&p.stop, ::aiz::dyn::cuda::CU_EVENT_DEFAULT);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    (void)cu->cuEventDestroy_v2(p.start);
    err = "cuEventCreate(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }
  return p;
}

static void destroyEvents(const ::aiz::dyn::cuda::Api* cu, CudaEventPair& p) {
  if (p.start) (void)cu->cuEventDestroy_v2(p.start);
  if (p.stop) (void)cu->cuEventDestroy_v2(p.stop);
  p.start = nullptr;
  p.stop = nullptr;
}

static std::optional<double> measureMemcpyGBps(const ::aiz::dyn::cuda::Api* cu,
                                              ::aiz::dyn::cuda::CUstream stream,
                                              ::aiz::dyn::cuda::CUdeviceptr dev,
                                              void* host,
                                              std::size_t bytes,
                                              bool h2d,
                                              int iters,
                                              int warmupIters,
                                              std::string& err) {
  // Warmup.
  for (int i = 0; i < warmupIters; ++i) {
    const auto r = h2d ? cu->cuMemcpyHtoDAsync_v2(dev, host, bytes, stream)
                       : cu->cuMemcpyDtoHAsync_v2(host, dev, bytes, stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = std::string("cuMemcpy") + (h2d ? "HtoD" : "DtoH") + "Async(warmup) failed: " + ::aiz::dyn::cuda::errToString(r);
      return std::nullopt;
    }
  }
  auto r = cu->cuStreamSynchronize(stream);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuStreamSynchronize(warmup) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  auto eventsOpt = createEvents(cu, err);
  if (!eventsOpt) return std::nullopt;
  CudaEventPair events = *eventsOpt;

  r = cu->cuEventRecord(events.start, stream);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    destroyEvents(cu, events);
    err = "cuEventRecord(start) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  for (int i = 0; i < iters; ++i) {
    r = h2d ? cu->cuMemcpyHtoDAsync_v2(dev, host, bytes, stream)
            : cu->cuMemcpyDtoHAsync_v2(host, dev, bytes, stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      destroyEvents(cu, events);
      err = std::string("cuMemcpy") + (h2d ? "HtoD" : "DtoH") + "Async(timed) failed: " + ::aiz::dyn::cuda::errToString(r);
      return std::nullopt;
    }
  }

  r = cu->cuEventRecord(events.stop, stream);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    destroyEvents(cu, events);
    err = "cuEventRecord(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  r = cu->cuEventSynchronize(events.stop);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    destroyEvents(cu, events);
    err = "cuEventSynchronize(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  float ms = 0.0f;
  r = cu->cuEventElapsedTime(&ms, events.start, events.stop);
  destroyEvents(cu, events);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuEventElapsedTime failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  const double sec = static_cast<double>(ms) / 1000.0;
  if (sec <= 0.0) {
    err = "timing failed";
    return std::nullopt;
  }

  const double totalBytes = static_cast<double>(bytes) * static_cast<double>(iters);
  const double gbps = (totalBytes / sec) / 1e9;
  return gbps;
}

class CudaPcieBandwidthPerGpu final : public IBenchmark {
public:
  explicit CudaPcieBandwidthPerGpu(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

  std::string name() const override { return "CUDA PCIe bandwidth"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = ::aiz::dyn::cuda::api(&err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    const auto* cu = ::aiz::dyn::cuda::api(&err);
    if (!cu) return BenchResult{false, err};

    int n = 0;
    auto r = cu->cuDeviceGetCount(&n);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuDeviceGetCount", r);
    if (n <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(n)) return BenchResult{false, "Invalid GPU index."};

    constexpr std::size_t bytes = 256ull * 1024ull * 1024ull;
    constexpr int warmupIters = 2;
    constexpr int iters = 10;

    ::aiz::dyn::cuda::CUdevice dev = 0;
    r = cu->cuDeviceGet(&dev, static_cast<int>(gpuIndex_));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuDeviceGet", r);

    ::aiz::dyn::cuda::CUcontext ctx{};
    r = cu->cuCtxCreate_v2(&ctx, 0, dev);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuCtxCreate_v2", r);

    void* host = nullptr;
    r = cu->cuMemHostAlloc(&host, bytes, ::aiz::dyn::cuda::CU_MEMHOSTALLOC_PORTABLE);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      (void)cu->cuCtxDestroy_v2(ctx);
      return failCuda("cuMemHostAlloc", r);
    }

    ::aiz::dyn::cuda::CUdeviceptr devmem{};
    r = cu->cuMemAlloc_v2(&devmem, bytes);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      (void)cu->cuMemFreeHost(host);
      (void)cu->cuCtxDestroy_v2(ctx);
      return failCuda("cuMemAlloc_v2", r);
    }

    ::aiz::dyn::cuda::CUstream stream{};
    r = cu->cuStreamCreate(&stream, ::aiz::dyn::cuda::CU_STREAM_NON_BLOCKING);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      (void)cu->cuMemFree_v2(devmem);
      (void)cu->cuMemFreeHost(host);
      (void)cu->cuCtxDestroy_v2(ctx);
      return failCuda("cuStreamCreate", r);
    }

    const auto rx = measureMemcpyGBps(cu, stream, devmem, host, bytes, true, iters, warmupIters, err);
    if (!rx) {
      (void)cu->cuStreamDestroy_v2(stream);
      (void)cu->cuMemFree_v2(devmem);
      (void)cu->cuMemFreeHost(host);
      (void)cu->cuCtxDestroy_v2(ctx);
      return BenchResult{false, "RX failed: " + err};
    }

    err.clear();
    const auto tx = measureMemcpyGBps(cu, stream, devmem, host, bytes, false, iters, warmupIters, err);
    if (!tx) {
      (void)cu->cuStreamDestroy_v2(stream);
      (void)cu->cuMemFree_v2(devmem);
      (void)cu->cuMemFreeHost(host);
      (void)cu->cuCtxDestroy_v2(ctx);
      return BenchResult{false, "TX failed: " + err};
    }

    (void)cu->cuStreamDestroy_v2(stream);
    (void)cu->cuMemFree_v2(devmem);
    (void)cu->cuMemFreeHost(host);
    (void)cu->cuCtxDestroy_v2(ctx);

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "RX: " << *rx << " GB/s, TX: " << *tx << " GB/s";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<CudaPcieBandwidthPerGpu>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_CUDA
