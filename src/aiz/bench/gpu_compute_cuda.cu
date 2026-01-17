#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
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

template <typename T>
__global__ void fmaKernel(T* out, int iters) {
  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  T a = static_cast<T>(tid % 97 + 1);
  T b = static_cast<T>(tid % 89 + 2);
  T c = static_cast<T>(1);
  #pragma unroll 4
  for (int i = 0; i < iters; ++i) {
    a = a * b + c;
    b = b * a + c;
  }
  out[tid] = a + b;
}

__global__ void fmaKernelHalf2(half2* out, int iters) {
  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  const half2 one = __float2half2_rn(1.0f);
  half2 a = __float2half2_rn(float((tid % 97) + 1));
  half2 b = __float2half2_rn(float((tid % 89) + 2));
  #pragma unroll 4
  for (int i = 0; i < iters; ++i) {
    a = __hfma2(a, b, one);
    b = __hfma2(b, a, one);
  }
  out[tid] = __hadd2(a, b);
}

__global__ void intKernel(int* out, int iters) {
  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int a = (tid % 97) + 1;
  int b = (tid % 89) + 2;
  int c = 1;
  #pragma unroll 4
  for (int i = 0; i < iters; ++i) {
    a = a * b + c;
    b = b * a + c;
  }
  out[tid] = a + b;
}

__global__ void int4Kernel(int* out, int iters) {
  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  // Two packed 8x int4 vectors (nibbles). Values are stored as unsigned 0..15.
  // We reinterpret as signed by subtracting 8.
  std::uint32_t aPack = 0x1234abcdU ^ static_cast<std::uint32_t>(tid * 2654435761u);
  std::uint32_t bPack = 0x0f1e2d3cU ^ static_cast<std::uint32_t>(tid * 2246822519u);

  int acc = 1;
  #pragma unroll 2
  for (int i = 0; i < iters; ++i) {
    // 8 lanes per pack.
    #pragma unroll
    for (int lane = 0; lane < 8; ++lane) {
      const int a4 = static_cast<int>((aPack >> (lane * 4)) & 0xF) - 8;
      const int b4 = static_cast<int>((bPack >> (lane * 4)) & 0xF) - 8;
      acc += a4 * b4;
    }
    aPack = (aPack << 1) | (aPack >> 31);
    bPack = (bPack << 3) | (bPack >> 29);
  }

  out[tid] = acc;
}

class GpuComputeBenchBase : public IBenchmark {
public:
  explicit GpuComputeBenchBase(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

protected:
  BenchResult prepDevice(int& deviceCount, cudaDeviceProp& prop) const {
    deviceCount = 0;
    cudaError_t e = cudaGetDeviceCount(&deviceCount);
    if (e != cudaSuccess) return failCuda("cudaGetDeviceCount", e);
    if (deviceCount <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(deviceCount)) return BenchResult{false, "Invalid GPU index."};

    e = cudaSetDevice(static_cast<int>(gpuIndex_));
    if (e != cudaSuccess) return failCuda("cudaSetDevice", e);

    e = cudaGetDeviceProperties(&prop, static_cast<int>(gpuIndex_));
    if (e != cudaSuccess) return failCuda("cudaGetDeviceProperties", e);

    return BenchResult{true, ""};
  }

  unsigned int gpuIndex_ = 0;
};

class GpuFp32Bench final : public GpuComputeBenchBase {
public:
  explicit GpuFp32Bench(unsigned int gpuIndex) : GpuComputeBenchBase(gpuIndex) {}
  std::string name() const override { return "CUDA FP32"; }
  bool isAvailable() const override {
    int n = 0;
    return (cudaGetDeviceCount(&n) == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }
  BenchResult run() override {
    int n = 0;
    cudaDeviceProp prop{};
    const BenchResult prep = prepDevice(n, prop);
    if (!prep.ok) return prep;

    constexpr int threads = 256;
    constexpr int blocks = 256;
    constexpr int warmup = 2;
    constexpr int timed = 5;
    constexpr int iters = 2048;

    float* out = nullptr;
    const std::size_t count = static_cast<std::size_t>(threads) * static_cast<std::size_t>(blocks);
    cudaError_t e = cudaMalloc(&out, count * sizeof(float));
    if (e != cudaSuccess) return failCuda("cudaMalloc", e);

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(out);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    auto launch = [&](cudaStream_t s) {
      fmaKernel<float><<<blocks, threads, 0, s>>>(out, iters);
    };

    // Manual timing to avoid std::function overhead.
    for (int i = 0; i < warmup; ++i) launch(stream);
    cudaStreamSynchronize(stream);

    std::string err;
    auto ev = createEvents(err);
    if (!ev) {
      cudaStreamDestroy(stream);
      cudaFree(out);
      return BenchResult{false, "cudaEventCreate failed"};
    }

    cudaEventRecord(ev->start, stream);
    for (int i = 0; i < timed; ++i) launch(stream);
    cudaEventRecord(ev->stop, stream);
    cudaEventSynchronize(ev->stop);

    float msTotal = 0.0f;
    cudaEventElapsedTime(&msTotal, ev->start, ev->stop);
    destroyEvents(*ev);

    cudaStreamDestroy(stream);
    cudaFree(out);

    const double sec = (static_cast<double>(msTotal) / 1000.0);
    if (sec <= 0.0) return BenchResult{false, "timing failed"};

    // Two FMA-style lines per iter: a=a*b+c; b=b*a+c; each is mul+add = 2 FLOPs.
    const double flopsPerIterPerThread = 4.0;
    const double totalFlops = static_cast<double>(count) * static_cast<double>(iters) * flopsPerIterPerThread * static_cast<double>(timed);
    const double gflops = (totalFlops / sec) / 1e9;

    std::ostringstream outStr;
    outStr.setf(std::ios::fixed);
    outStr.precision(2);
    outStr << gflops << " GFLOPS";
    return BenchResult{true, outStr.str()};
  }
};

class GpuFp64Bench final : public GpuComputeBenchBase {
public:
  explicit GpuFp64Bench(unsigned int gpuIndex) : GpuComputeBenchBase(gpuIndex) {}
  std::string name() const override { return "CUDA FP64"; }
  bool isAvailable() const override {
    int n = 0;
    return (cudaGetDeviceCount(&n) == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }
  BenchResult run() override {
    int n = 0;
    cudaDeviceProp prop{};
    const BenchResult prep = prepDevice(n, prop);
    if (!prep.ok) return prep;

    // If the device has no FP64 capability exposed, still run but it will be slow.
    constexpr int threads = 256;
    constexpr int blocks = 256;
    constexpr int warmup = 2;
    constexpr int timed = 5;
    constexpr int iters = 1024;

    double* out = nullptr;
    const std::size_t count = static_cast<std::size_t>(threads) * static_cast<std::size_t>(blocks);
    cudaError_t e = cudaMalloc(&out, count * sizeof(double));
    if (e != cudaSuccess) return failCuda("cudaMalloc", e);

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(out);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    auto launch = [&](cudaStream_t s) {
      fmaKernel<double><<<blocks, threads, 0, s>>>(out, iters);
    };

    for (int i = 0; i < warmup; ++i) launch(stream);
    cudaStreamSynchronize(stream);

    std::string err;
    auto ev = createEvents(err);
    if (!ev) {
      cudaStreamDestroy(stream);
      cudaFree(out);
      return BenchResult{false, "cudaEventCreate failed"};
    }

    cudaEventRecord(ev->start, stream);
    for (int i = 0; i < timed; ++i) launch(stream);
    cudaEventRecord(ev->stop, stream);
    cudaEventSynchronize(ev->stop);

    float msTotal = 0.0f;
    cudaEventElapsedTime(&msTotal, ev->start, ev->stop);
    destroyEvents(*ev);

    cudaStreamDestroy(stream);
    cudaFree(out);

    const double sec = (static_cast<double>(msTotal) / 1000.0);
    if (sec <= 0.0) return BenchResult{false, "timing failed"};

    const double flopsPerIterPerThread = 4.0;
    const double totalFlops = static_cast<double>(count) * static_cast<double>(iters) * flopsPerIterPerThread * static_cast<double>(timed);
    const double gflops = (totalFlops / sec) / 1e9;

    std::ostringstream outStr;
    outStr.setf(std::ios::fixed);
    outStr.precision(2);
    outStr << gflops << " GFLOPS";
    return BenchResult{true, outStr.str()};
  }
};

class GpuFp16Bench final : public GpuComputeBenchBase {
public:
  explicit GpuFp16Bench(unsigned int gpuIndex) : GpuComputeBenchBase(gpuIndex) {}
  std::string name() const override { return "CUDA FP16"; }
  bool isAvailable() const override {
    int n = 0;
    return (cudaGetDeviceCount(&n) == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }
  BenchResult run() override {
    int n = 0;
    cudaDeviceProp prop{};
    const BenchResult prep = prepDevice(n, prop);
    if (!prep.ok) return prep;

    constexpr int threads = 256;
    constexpr int blocks = 256;
    constexpr int warmup = 2;
    constexpr int timed = 5;
    constexpr int iters = 4096;

    half2* out = nullptr;
    const std::size_t count = static_cast<std::size_t>(threads) * static_cast<std::size_t>(blocks);
    cudaError_t e = cudaMalloc(&out, count * sizeof(half2));
    if (e != cudaSuccess) return failCuda("cudaMalloc", e);

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(out);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    auto launch = [&](cudaStream_t s) {
      fmaKernelHalf2<<<blocks, threads, 0, s>>>(out, iters);
    };

    for (int i = 0; i < warmup; ++i) launch(stream);
    cudaStreamSynchronize(stream);

    std::string err;
    auto ev = createEvents(err);
    if (!ev) {
      cudaStreamDestroy(stream);
      cudaFree(out);
      return BenchResult{false, "cudaEventCreate failed"};
    }

    cudaEventRecord(ev->start, stream);
    for (int i = 0; i < timed; ++i) launch(stream);
    cudaEventRecord(ev->stop, stream);
    cudaEventSynchronize(ev->stop);

    float msTotal = 0.0f;
    cudaEventElapsedTime(&msTotal, ev->start, ev->stop);
    destroyEvents(*ev);

    cudaStreamDestroy(stream);
    cudaFree(out);

    const double sec = (static_cast<double>(msTotal) / 1000.0);
    if (sec <= 0.0) return BenchResult{false, "timing failed"};

    // In-kernel per-iter:
    // a = hfma2(a,b,one); b = hfma2(b,a,one)
    // each hfma2 is 2 lanes * (mul+add) = 4 FLOPs, so per iter = 8 FLOPs.
    const double flopsPerIterPerThread = 8.0;
    const double totalFlops = static_cast<double>(count) * static_cast<double>(iters) * flopsPerIterPerThread * static_cast<double>(timed);
    const double gflops = (totalFlops / sec) / 1e9;

    std::ostringstream outStr;
    outStr.setf(std::ios::fixed);
    outStr.precision(2);
    outStr << gflops << " GFLOPS";
    return BenchResult{true, outStr.str()};
  }
};

class GpuInt8Bench final : public GpuComputeBenchBase {
public:
  explicit GpuInt8Bench(unsigned int gpuIndex) : GpuComputeBenchBase(gpuIndex) {}
  std::string name() const override { return "CUDA INT8"; }
  bool isAvailable() const override {
    int n = 0;
    return (cudaGetDeviceCount(&n) == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }
  BenchResult run() override {
    int n = 0;
    cudaDeviceProp prop{};
    const BenchResult prep = prepDevice(n, prop);
    if (!prep.ok) return prep;

    constexpr int threads = 256;
    constexpr int blocks = 256;
    constexpr int warmup = 2;
    constexpr int timed = 5;
    constexpr int iters = 4096;

    int* out = nullptr;
    const std::size_t count = static_cast<std::size_t>(threads) * static_cast<std::size_t>(blocks);
    cudaError_t e = cudaMalloc(&out, count * sizeof(int));
    if (e != cudaSuccess) return failCuda("cudaMalloc", e);

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(out);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    auto launch = [&](cudaStream_t s) {
      intKernel<<<blocks, threads, 0, s>>>(out, iters);
    };

    for (int i = 0; i < warmup; ++i) launch(stream);
    cudaStreamSynchronize(stream);

    std::string err;
    auto ev = createEvents(err);
    if (!ev) {
      cudaStreamDestroy(stream);
      cudaFree(out);
      return BenchResult{false, "cudaEventCreate failed"};
    }

    cudaEventRecord(ev->start, stream);
    for (int i = 0; i < timed; ++i) launch(stream);
    cudaEventRecord(ev->stop, stream);
    cudaEventSynchronize(ev->stop);

    float msTotal = 0.0f;
    cudaEventElapsedTime(&msTotal, ev->start, ev->stop);
    destroyEvents(*ev);

    cudaStreamDestroy(stream);
    cudaFree(out);

    const double sec = (static_cast<double>(msTotal) / 1000.0);
    if (sec <= 0.0) return BenchResult{false, "timing failed"};

    const double opsPerIterPerThread = 4.0;  // 2x (mul+add)
    const double totalOps = static_cast<double>(count) * static_cast<double>(iters) * opsPerIterPerThread * static_cast<double>(timed);
    const double gops = (totalOps / sec) / 1e9;

    std::ostringstream outStr;
    outStr.setf(std::ios::fixed);
    outStr.precision(2);
    outStr << gops << " GOPS";
    return BenchResult{true, outStr.str()};
  }
};

class GpuInt4Bench final : public IBenchmark {
public:
  explicit GpuInt4Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA INT4"; }
  bool isAvailable() const override {
    int n = 0;
    return (cudaGetDeviceCount(&n) == cudaSuccess) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }
  BenchResult run() override {
    int n = 0;
    cudaDeviceProp prop{};
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) return failCuda("cudaGetDeviceCount", e);
    if (n <= 0) return BenchResult{false, "No CUDA devices found."};
    if (gpuIndex_ >= static_cast<unsigned int>(n)) return BenchResult{false, "Invalid GPU index."};

    e = cudaSetDevice(static_cast<int>(gpuIndex_));
    if (e != cudaSuccess) return failCuda("cudaSetDevice", e);

    e = cudaGetDeviceProperties(&prop, static_cast<int>(gpuIndex_));
    if (e != cudaSuccess) return failCuda("cudaGetDeviceProperties", e);

    constexpr int threads = 256;
    constexpr int blocks = 256;
    constexpr int warmup = 2;
    constexpr int timed = 5;
    constexpr int iters = 2048;

    int* out = nullptr;
    const std::size_t count = static_cast<std::size_t>(threads) * static_cast<std::size_t>(blocks);
    e = cudaMalloc(&out, count * sizeof(int));
    if (e != cudaSuccess) return failCuda("cudaMalloc", e);

    cudaStream_t stream{};
    e = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
      cudaFree(out);
      return failCuda("cudaStreamCreateWithFlags", e);
    }

    auto launch = [&](cudaStream_t s) {
      int4Kernel<<<blocks, threads, 0, s>>>(out, iters);
    };

    for (int i = 0; i < warmup; ++i) launch(stream);
    cudaStreamSynchronize(stream);

    std::string err;
    auto ev = createEvents(err);
    if (!ev) {
      cudaStreamDestroy(stream);
      cudaFree(out);
      return BenchResult{false, "cudaEventCreate failed"};
    }

    cudaEventRecord(ev->start, stream);
    for (int i = 0; i < timed; ++i) launch(stream);
    cudaEventRecord(ev->stop, stream);
    cudaEventSynchronize(ev->stop);

    float msTotal = 0.0f;
    cudaEventElapsedTime(&msTotal, ev->start, ev->stop);
    destroyEvents(*ev);

    cudaStreamDestroy(stream);
    cudaFree(out);

    const double sec = (static_cast<double>(msTotal) / 1000.0);
    if (sec <= 0.0) return BenchResult{false, "timing failed"};

    // Per iter: 8 lanes, each does (mul+add) => 2 ops => 16 ops.
    const double opsPerIterPerThread = 16.0;
    const double totalOps = static_cast<double>(count) * static_cast<double>(iters) * opsPerIterPerThread * static_cast<double>(timed);
    const double gops = (totalOps / sec) / 1e9;

    std::ostringstream outStr;
    outStr.setf(std::ios::fixed);
    outStr.precision(2);
    outStr << gops << " GOPS";
    return BenchResult{true, outStr.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuFp16BenchmarkCuda(unsigned int gpuIndex) { return std::make_unique<GpuFp16Bench>(gpuIndex); }
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkCuda(unsigned int gpuIndex) { return std::make_unique<GpuFp32Bench>(gpuIndex); }
std::unique_ptr<IBenchmark> makeGpuFp64BenchmarkCuda(unsigned int gpuIndex) { return std::make_unique<GpuFp64Bench>(gpuIndex); }
std::unique_ptr<IBenchmark> makeGpuInt4BenchmarkCuda(unsigned int gpuIndex) { return std::make_unique<GpuInt4Bench>(gpuIndex); }
std::unique_ptr<IBenchmark> makeGpuInt8BenchmarkCuda(unsigned int gpuIndex) { return std::make_unique<GpuInt8Bench>(gpuIndex); }

}  // namespace aiz

#endif  // AI_Z_ENABLE_CUDA
