#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <cuda_runtime_api.h>

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

static std::string cudaErrToString(cudaError_t e) {
  const char* s = cudaGetErrorString(e);
  return s ? std::string{s} : std::string{"<unknown cuda error>"};
}

static BenchResult failCuda(const std::string& where, cudaError_t e) {
  return BenchResult{false, where + ": " + cudaErrToString(e)};
}

struct CudaEventPair {
  cudaEvent_t start{};
  cudaEvent_t stop{};
};

static std::optional<CudaEventPair> createEvents(std::string& err) {
  CudaEventPair p;
  if (cudaEventCreate(&p.start) != cudaSuccess) {
    err = "cudaEventCreate(start) failed";
    return std::nullopt;
  }
  if (cudaEventCreate(&p.stop) != cudaSuccess) {
    cudaEventDestroy(p.start);
    err = "cudaEventCreate(stop) failed";
    return std::nullopt;
  }
  return p;
}

static void destroyEvents(CudaEventPair& p) {
  if (p.start) cudaEventDestroy(p.start);
  if (p.stop) cudaEventDestroy(p.stop);
  p.start = nullptr;
  p.stop = nullptr;
}

static std::optional<double> measureMemcpyGBps(cudaStream_t stream,
                                              void* dst,
                                              const void* src,
                                              std::size_t bytes,
                                              cudaMemcpyKind kind,
                                              int iters,
                                              int warmupIters,
                                              std::string& err) {
  // Warmup
  for (int i = 0; i < warmupIters; ++i) {
    const cudaError_t e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    if (e != cudaSuccess) {
      err = "cudaMemcpyAsync(warmup) failed: " + cudaErrToString(e);
      return std::nullopt;
    }
  }
  if (cudaStreamSynchronize(stream) != cudaSuccess) {
    err = "cudaStreamSynchronize(warmup) failed";
    return std::nullopt;
  }

  auto eventsOpt = createEvents(err);
  if (!eventsOpt) return std::nullopt;
  CudaEventPair events = *eventsOpt;

  cudaError_t e = cudaEventRecord(events.start, stream);
  if (e != cudaSuccess) {
    destroyEvents(events);
    err = "cudaEventRecord(start) failed: " + cudaErrToString(e);
    return std::nullopt;
  }

  for (int i = 0; i < iters; ++i) {
    e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    if (e != cudaSuccess) {
      destroyEvents(events);
      err = "cudaMemcpyAsync(timed) failed: " + cudaErrToString(e);
      return std::nullopt;
    }
  }

  e = cudaEventRecord(events.stop, stream);
  if (e != cudaSuccess) {
    destroyEvents(events);
    err = "cudaEventRecord(stop) failed: " + cudaErrToString(e);
    return std::nullopt;
  }

  e = cudaEventSynchronize(events.stop);
  if (e != cudaSuccess) {
    destroyEvents(events);
    err = "cudaEventSynchronize(stop) failed: " + cudaErrToString(e);
    return std::nullopt;
  }

  float ms = 0.0f;
  e = cudaEventElapsedTime(&ms, events.start, events.stop);
  destroyEvents(events);
  if (e != cudaSuccess) {
    err = "cudaEventElapsedTime failed: " + cudaErrToString(e);
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
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    return (e == cudaSuccess) && (n > 0);
  }

  BenchResult run() override {
    int deviceCount = 0;
    cudaError_t e = cudaGetDeviceCount(&deviceCount);
    if (e != cudaSuccess) return failCuda("cudaGetDeviceCount", e);
    if (deviceCount <= 0) return BenchResult{false, "No CUDA devices found."};

    // Use a reasonably large buffer to approach peak throughput.
    // 256 MiB tends to be enough without being too slow on smaller systems.
    constexpr std::size_t bytes = 256ull * 1024ull * 1024ull;
    constexpr int warmupIters = 2;
    constexpr int iters = 10;

    void* host = nullptr;
    e = cudaMallocHost(&host, bytes);
    if (e != cudaSuccess) return failCuda("cudaMallocHost", e);

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    for (int device = 0; device < deviceCount; ++device) {
      e = cudaSetDevice(device);
      if (e != cudaSuccess) {
        cudaFreeHost(host);
        return failCuda("cudaSetDevice", e);
      }

      cudaDeviceProp prop{};
      e = cudaGetDeviceProperties(&prop, device);
      if (e != cudaSuccess) {
        cudaFreeHost(host);
        return failCuda("cudaGetDeviceProperties", e);
      }

      void* dev = nullptr;
      e = cudaMalloc(&dev, bytes);
      if (e != cudaSuccess) {
        cudaFreeHost(host);
        return failCuda("cudaMalloc", e);
      }

      cudaStream_t stream{};
      e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
      if (e != cudaSuccess) {
        cudaFree(dev);
        cudaFreeHost(host);
        return failCuda("cudaStreamCreateWithFlags", e);
      }

      std::string err;
      const auto h2d = measureMemcpyGBps(stream, dev, host, bytes, cudaMemcpyHostToDevice, iters, warmupIters, err);
      if (!h2d) {
        cudaStreamDestroy(stream);
        cudaFree(dev);
        cudaFreeHost(host);
        return BenchResult{false, "GPU" + std::to_string(device) + " H→D failed: " + err};
      }

      err.clear();
      const auto d2h = measureMemcpyGBps(stream, host, dev, bytes, cudaMemcpyDeviceToHost, iters, warmupIters, err);
      if (!d2h) {
        cudaStreamDestroy(stream);
        cudaFree(dev);
        cudaFreeHost(host);
        return BenchResult{false, "GPU" + std::to_string(device) + " D→H failed: " + err};
      }

      cudaStreamDestroy(stream);
      cudaFree(dev);

          if (device > 0) out << "\n";
          // Per-GPU block (UI indents each line). Keep RX/TX on separate lines so values align.
          out << "GPU" << device << ": " << prop.name << "\n"
            << "RX: " << *h2d << " GB/s\n"
            << "TX: " << *d2h << " GB/s";
    }

    cudaFreeHost(host);
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
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    return (e == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    int deviceCount = 0;
    cudaError_t e = cudaGetDeviceCount(&deviceCount);
    if (e != cudaSuccess) return failCuda("cudaGetDeviceCount", e);
    if (deviceCount <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(deviceCount)) return BenchResult{false, "Invalid GPU index."};

    constexpr std::size_t bytes = 256ull * 1024ull * 1024ull;
    constexpr int warmupIters = 2;
    constexpr int iters = 10;

    e = cudaSetDevice(static_cast<int>(gpuIndex_));
    if (e != cudaSuccess) return failCuda("cudaSetDevice", e);

    void* host = nullptr;
    e = cudaMallocHost(&host, bytes);
    if (e != cudaSuccess) return failCuda("cudaMallocHost", e);

    void* dev = nullptr;
    e = cudaMalloc(&dev, bytes);
    if (e != cudaSuccess) {
      cudaFreeHost(host);
      return failCuda("cudaMalloc", e);
    }

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(dev);
      cudaFreeHost(host);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    std::string err;
    std::optional<double> gbps;
    if (dir_ == Dir::Rx) {
      // RX: host -> device (GPU receives).
      gbps = measureMemcpyGBps(stream, dev, host, bytes, cudaMemcpyHostToDevice, iters, warmupIters, err);
    } else {
      // TX: device -> host (GPU sends).
      gbps = measureMemcpyGBps(stream, host, dev, bytes, cudaMemcpyDeviceToHost, iters, warmupIters, err);
    }

    cudaStreamDestroy(stream);
    cudaFree(dev);
    cudaFreeHost(host);

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
