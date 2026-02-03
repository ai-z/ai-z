#include <aiz/bench/bench.h>
#include <aiz/bench/factory.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>

namespace aiz {

// Forward declarations for ONNX Runtime benchmarks
std::unique_ptr<IBenchmark> makeOrtCpuMatMulBenchmark();
std::unique_ptr<IBenchmark> makeOrtCpuMemoryBandwidthBenchmark();

#ifdef AI_Z_ENABLE_CUDA
std::unique_ptr<IBenchmark> makePcieBandwidthBenchmarkCuda();
std::unique_ptr<IBenchmark> makePcieBandwidthRxBenchmarkCuda(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makePcieBandwidthTxBenchmarkCuda(unsigned int gpuIndex);

std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkCuda(unsigned int gpuIndex);

std::unique_ptr<IBenchmark> makeGpuFp16BenchmarkCuda(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkCuda(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp64BenchmarkCuda(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt4BenchmarkCuda(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt8BenchmarkCuda(unsigned int gpuIndex);
#endif

#ifdef AI_Z_ENABLE_OPENCL
std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkOpenclBackend(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkOpenclBackend(unsigned int gpuIndex);
#endif

#ifdef AI_Z_ENABLE_VULKAN
std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkVulkanBackend(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkVulkanBackend(unsigned int gpuIndex);
#endif

namespace {

class FlopsStub final : public IBenchmark {
public:
  explicit FlopsStub(std::string label) : label_(std::move(label)) {}

  std::string name() const override { return label_; }
  bool isAvailable() const override { return true; }
  BenchResult run() override {
    // Minimal CPU FLOPS-ish loop.
    constexpr std::size_t iters = 200'000'000;
    volatile float x = 1.0f;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
      x = x * 1.0000001f + 0.0000001f;
    }
    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const double ops = static_cast<double>(iters) * 2.0;  // mul+add
    const double gflops = (ops / sec) / 1e9;
    {
      std::ostringstream oss;
      oss.setf(std::ios::fixed);
      oss.precision(2);
      oss << gflops;
      return BenchResult{true, oss.str() + " GFLOPS"};
    }
  }

private:
  std::string label_;
};

class FlopsFp16Stub final : public IBenchmark {
public:
  std::string name() const override { return "FLOPS FP16"; }
  bool isAvailable() const override {
#if defined(__FLT16_MANT_DIG__)
    return true;
#else
    return false;
#endif
  }
  BenchResult run() override {
#if !defined(__FLT16_MANT_DIG__)
    return BenchResult{false, "FP16 not supported by this compiler/target."};
#else
    // Best-effort scalar loop (not vectorized; primarily for relative sanity).
    constexpr std::size_t iters = 200'000'000;
    volatile _Float16 x = static_cast<_Float16>(1.0);
    const _Float16 a = static_cast<_Float16>(1.0000001);
    const _Float16 b = static_cast<_Float16>(0.0000001);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
      x = static_cast<_Float16>(x * a + b);
    }
    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const double ops = static_cast<double>(iters) * 2.0;
    const double gflops = (ops / sec) / 1e9;
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(2);
    oss << gflops;
    return BenchResult{true, oss.str() + " GFLOPS"};
#endif
  }
};

class UnavailableStub final : public IBenchmark {
public:
  UnavailableStub(std::string name, std::string why)
      : name_(std::move(name)), why_(std::move(why)) {}
  std::string name() const override { return name_; }
  bool isAvailable() const override { return false; }
  BenchResult run() override { return BenchResult{false, why_}; }

private:
  std::string name_;
  std::string why_;
};

class PcieBandwidthStub final : public IBenchmark {
public:
  std::string name() const override { return "PCIe bandwidth test"; }
  bool isAvailable() const override { return false; }
  BenchResult run() override {
    return BenchResult{false,
                       "Not built with a PCIe transfer backend. Reconfigure with -DAI_Z_ENABLE_CUDA=ON (NVIDIA), "
                       "or enable HIP/SYCL in a future pass."};
  }
};

class TorchMatmulStub final : public IBenchmark {
public:
  std::string name() const override { return "Matrix multiplication (PyTorch C++ API)"; }
  bool isAvailable() const override {
#ifdef AI_Z_HAS_TORCH
    return true;
#else
    return false;
#endif
  }
  BenchResult run() override {
#ifdef AI_Z_HAS_TORCH
    return BenchResult{true, "LibTorch enabled (implementation TODO)."};
#else
    return BenchResult{false, "Built without LibTorch."};
#endif
  }
};

}  // namespace

// Simple factory helpers for UI.
std::unique_ptr<IBenchmark> makePcieBandwidthBenchmark() {
#ifdef AI_Z_ENABLE_CUDA
  return makePcieBandwidthBenchmarkCuda();
#else
  return std::make_unique<PcieBandwidthStub>();
#endif
}

std::unique_ptr<IBenchmark> makeFlopsBenchmark() {
  // Backwards-compatible legacy entry.
  return std::make_unique<FlopsStub>("Floating point per second");
}

std::unique_ptr<IBenchmark> makeTorchMatmulBenchmark() { return std::make_unique<TorchMatmulStub>(); }

std::unique_ptr<IBenchmark> makeGpuPcieRxBenchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makePcieBandwidthRxBenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("PCIe RX", "Not built with a PCIe transfer backend. Reconfigure with -DAI_Z_ENABLE_CUDA=ON.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuPcieTxBenchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makePcieBandwidthTxBenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("PCIe TX", "Not built with a PCIe transfer backend. Reconfigure with -DAI_Z_ENABLE_CUDA=ON.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuCudaPcieBandwidthBenchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuPcieBandwidthBenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("CUDA PCIe bandwidth", "Not built with CUDA backend. Reconfigure with -DAI_Z_ENABLE_CUDA=ON.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuVulkanPcieBandwidthBenchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_VULKAN
  return makeGpuPcieBandwidthBenchmarkVulkanBackend(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>(
      "Vulkan PCIe bandwidth",
      "Not built with Vulkan backend. Reconfigure with -DAI_Z_ENABLE_VULKAN=ON (and install Vulkan headers/loader dev packages, e.g. libvulkan-dev)." );
#endif
}

std::unique_ptr<IBenchmark> makeGpuOpenclPcieBandwidthBenchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_OPENCL
  return makeGpuPcieBandwidthBenchmarkOpenclBackend(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>(
      "OpenCL PCIe bandwidth",
      "Not built with OpenCL backend. Reconfigure with -DAI_Z_ENABLE_OPENCL=ON (and install OpenCL headers/ICD loader dev packages, e.g. ocl-icd-opencl-dev)." );
#endif
}

std::unique_ptr<IBenchmark> makeGpuFp16Benchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuFp16BenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("FP16", "Not built with CUDA backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuFp32Benchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuFp32BenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("FP32", "Not built with CUDA backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkVulkan(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_VULKAN
  return makeGpuFp32BenchmarkVulkanBackend(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("Vulkan FLOPS FP32", "Not built with Vulkan backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkOpencl(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_OPENCL
  return makeGpuFp32BenchmarkOpenclBackend(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("OpenCL FLOPS FP32", "Not built with OpenCL backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuFp64Benchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuFp64BenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("FP64", "Not built with CUDA backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuInt4Benchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuInt4BenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("INT4", "Not built with CUDA backend.");
#endif
}

std::unique_ptr<IBenchmark> makeGpuInt8Benchmark(unsigned int gpuIndex) {
#ifdef AI_Z_ENABLE_CUDA
  return makeGpuInt8BenchmarkCuda(gpuIndex);
#else
  (void)gpuIndex;
  return std::make_unique<UnavailableStub>("INT8", "Not built with CUDA backend.");
#endif
}

std::unique_ptr<IBenchmark> makeCpuFp16FlopsBenchmark() { return std::make_unique<FlopsFp16Stub>(); }
std::unique_ptr<IBenchmark> makeCpuFp32FlopsBenchmark() { return std::make_unique<FlopsStub>("FLOPS FP32"); }

}  // namespace aiz
