#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <cuda_runtime_api.h>

#include <cstddef>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

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
  // Warmup.
  for (int i = 0; i < warmupIters; ++i) {
    const cudaError_t e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    if (e != cudaSuccess) {
      err = "cudaMemcpyAsync(warmup) failed: " + cudaErrToString(e);
      return std::nullopt;
    }
  }
  cudaError_t e = cudaStreamSynchronize(stream);
  if (e != cudaSuccess) {
    err = "cudaStreamSynchronize(warmup) failed: " + cudaErrToString(e);
    return std::nullopt;
  }

  auto eventsOpt = createEvents(err);
  if (!eventsOpt) return std::nullopt;
  CudaEventPair events = *eventsOpt;

  e = cudaEventRecord(events.start, stream);
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
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    return (e == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    int n = 0;
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) return failCuda("cudaGetDeviceCount", e);
    if (n <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(n)) return BenchResult{false, "Invalid GPU index."};

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
    const auto rx = measureMemcpyGBps(stream, dev, host, bytes, cudaMemcpyHostToDevice, iters, warmupIters, err);
    if (!rx) {
      cudaStreamDestroy(stream);
      cudaFree(dev);
      cudaFreeHost(host);
      return BenchResult{false, "RX failed: " + err};
    }

    err.clear();
    const auto tx = measureMemcpyGBps(stream, host, dev, bytes, cudaMemcpyDeviceToHost, iters, warmupIters, err);
    if (!tx) {
      cudaStreamDestroy(stream);
      cudaFree(dev);
      cudaFreeHost(host);
      return BenchResult{false, "TX failed: " + err};
    }

    cudaStreamDestroy(stream);
    cudaFree(dev);
    cudaFreeHost(host);

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
