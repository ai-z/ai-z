#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <aiz/dyn/cuda.h>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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
  for (int i = 0; i < warmupIters; ++i) {
    const auto r = h2d ? cu->cuMemcpyHtoDAsync_v2(dev, host, bytes, stream)
                       : cu->cuMemcpyDtoHAsync_v2(host, dev, bytes, stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = std::string("cuMemcpy") + (h2d ? "HtoD" : "DtoH") + "Async(warmup) failed: " + ::aiz::dyn::cuda::errToString(r);
      return std::nullopt;
    }
  }
  if (cu->cuStreamSynchronize(stream) != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuStreamSynchronize(warmup) failed";
    return std::nullopt;
  }

  auto eventsOpt = createEvents(cu, err);
  if (!eventsOpt) return std::nullopt;
  CudaEventPair events = *eventsOpt;

  auto r = cu->cuEventRecord(events.start, stream);
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
    err = "timing produced non-positive duration";
    return std::nullopt;
  }

  const double totalBytes = static_cast<double>(bytes) * static_cast<double>(iters);
  const double gbps = (totalBytes / sec) / 1e9;
  return gbps;
}

class PcieBandwidthCuda final : public IBenchmark {
public:
  std::string name() const override { return "PCIe bandwidth test (CUDA)"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = ::aiz::dyn::cuda::api(&err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0);
  }

  BenchResult run() override {
    std::string err;
    const auto* cu = ::aiz::dyn::cuda::api(&err);
    if (!cu) return BenchResult{false, err};

    int deviceCount = 0;
    auto r = cu->cuDeviceGetCount(&deviceCount);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuDeviceGetCount", r);
    if (deviceCount <= 0) return BenchResult{false, "No CUDA devices found."};

    // Use a reasonably large buffer to approach peak throughput.
    // 256 MiB tends to be enough without being too slow on smaller systems.
    constexpr std::size_t bytes = 256ull * 1024ull * 1024ull;
    constexpr int warmupIters = 2;
    constexpr int iters = 10;

    void* host = nullptr;
    r = cu->cuMemHostAlloc(&host, bytes, ::aiz::dyn::cuda::CU_MEMHOSTALLOC_PORTABLE);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemHostAlloc", r);

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    for (int device = 0; device < deviceCount; ++device) {
      ::aiz::dyn::cuda::CUdevice dev = 0;
      r = cu->cuDeviceGet(&dev, device);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        (void)cu->cuMemFreeHost(host);
        return failCuda("cuDeviceGet", r);
      }

      char name[128] = {0};
      (void)cu->cuDeviceGetName(name, static_cast<int>(sizeof(name)), dev);

      ::aiz::dyn::cuda::CUcontext ctx{};
      r = cu->cuCtxCreate_v2(&ctx, 0, dev);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        (void)cu->cuMemFreeHost(host);
        return failCuda("cuCtxCreate_v2", r);
      }

      ::aiz::dyn::cuda::CUdeviceptr devmem{};
      r = cu->cuMemAlloc_v2(&devmem, bytes);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        (void)cu->cuCtxDestroy_v2(ctx);
        (void)cu->cuMemFreeHost(host);
        return failCuda("cuMemAlloc_v2", r);
      }

      ::aiz::dyn::cuda::CUstream stream{};
      r = cu->cuStreamCreate(&stream, ::aiz::dyn::cuda::CU_STREAM_NON_BLOCKING);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        (void)cu->cuMemFree_v2(devmem);
        (void)cu->cuCtxDestroy_v2(ctx);
        (void)cu->cuMemFreeHost(host);
        return failCuda("cuStreamCreate", r);
      }

      const auto h2d = measureMemcpyGBps(cu, stream, devmem, host, bytes, true, iters, warmupIters, err);
      if (!h2d) {
        (void)cu->cuStreamDestroy_v2(stream);
        (void)cu->cuMemFree_v2(devmem);
        (void)cu->cuCtxDestroy_v2(ctx);
        (void)cu->cuMemFreeHost(host);
        return BenchResult{false, "GPU" + std::to_string(device) + " H→D failed: " + err};
      }

      err.clear();
      const auto d2h = measureMemcpyGBps(cu, stream, devmem, host, bytes, false, iters, warmupIters, err);
      if (!d2h) {
        (void)cu->cuStreamDestroy_v2(stream);
        (void)cu->cuMemFree_v2(devmem);
        (void)cu->cuCtxDestroy_v2(ctx);
        (void)cu->cuMemFreeHost(host);
        return BenchResult{false, "GPU" + std::to_string(device) + " D→H failed: " + err};
      }

      (void)cu->cuStreamDestroy_v2(stream);
      (void)cu->cuMemFree_v2(devmem);
      (void)cu->cuCtxDestroy_v2(ctx);

          if (device > 0) out << "\n";
          // Per-GPU block (UI indents each line). Keep RX/TX on separate lines so values align.
          out << "GPU" << device << ": " << name << "\n"
            << "RX: " << *h2d << " GB/s\n"
            << "TX: " << *d2h << " GB/s";
    }

    (void)cu->cuMemFreeHost(host);
    return BenchResult{true, out.str()};
  }
};

class PcieBandwidthCudaOneDir final : public IBenchmark {
public:
  enum class Dir { Rx, Tx };

  PcieBandwidthCudaOneDir(unsigned int gpuIndex, Dir dir)
      : gpuIndex_(gpuIndex), dir_(dir) {}

  std::string name() const override {
    return (dir_ == Dir::Rx) ? "PCIe RX" : "PCIe TX";
  }

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

    int deviceCount = 0;
    auto r = cu->cuDeviceGetCount(&deviceCount);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuDeviceGetCount", r);
    if (deviceCount <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(deviceCount)) return BenchResult{false, "Invalid GPU index."};

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

    std::optional<double> gbps;
    if (dir_ == Dir::Rx) {
      // RX: host -> device (GPU receives).
      gbps = measureMemcpyGBps(cu, stream, devmem, host, bytes, true, iters, warmupIters, err);
    } else {
      // TX: device -> host (GPU sends).
      gbps = measureMemcpyGBps(cu, stream, devmem, host, bytes, false, iters, warmupIters, err);
    }

    (void)cu->cuStreamDestroy_v2(stream);
    (void)cu->cuMemFree_v2(devmem);
    (void)cu->cuMemFreeHost(host);
    (void)cu->cuCtxDestroy_v2(ctx);

    if (!gbps) return BenchResult{false, err.empty() ? std::string("transfer failed") : err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gbps << " GB/s";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
  Dir dir_ = Dir::Rx;
};

}  // namespace

std::unique_ptr<IBenchmark> makePcieBandwidthBenchmarkCuda() { return std::make_unique<PcieBandwidthCuda>(); }

std::unique_ptr<IBenchmark> makePcieBandwidthRxBenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<PcieBandwidthCudaOneDir>(gpuIndex, PcieBandwidthCudaOneDir::Dir::Rx);
}

std::unique_ptr<IBenchmark> makePcieBandwidthTxBenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<PcieBandwidthCudaOneDir>(gpuIndex, PcieBandwidthCudaOneDir::Dir::Tx);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_CUDA
