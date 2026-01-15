#include <aiz/bench/bench.h>
#include <aiz/bench/factory.h>

#include <chrono>
#include <memory>
#include <string>

namespace aiz {

namespace {

class FlopsStub final : public IBenchmark {
public:
  std::string name() const override { return "Floating point per second"; }
  bool isAvailable() const override { return true; }
  BenchResult run() override {
    // Minimal CPU FLOPS-ish loop (placeholder; will refine).
    constexpr std::size_t iters = 50'000'000;
    volatile double x = 1.0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
      x = x * 1.0000001 + 0.0000001;
    }
    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const double ops = static_cast<double>(iters) * 2.0;  // mul+add
    const double gops = (ops / sec) / 1e9;
    return BenchResult{true, "CPU loop ~" + std::to_string(gops) + " Gop/s"};
  }
};

class PcieBandwidthStub final : public IBenchmark {
public:
  std::string name() const override { return "PCIe bandwidth test"; }
  bool isAvailable() const override { return false; }
  BenchResult run() override { return BenchResult{false, "Not built with a PCIe transfer backend (CUDA/HIP/SYCL)."}; }
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
std::unique_ptr<IBenchmark> makePcieBandwidthBenchmark() { return std::make_unique<PcieBandwidthStub>(); }
std::unique_ptr<IBenchmark> makeFlopsBenchmark() { return std::make_unique<FlopsStub>(); }
std::unique_ptr<IBenchmark> makeTorchMatmulBenchmark() { return std::make_unique<TorchMatmulStub>(); }

}  // namespace aiz
