#include <aiz/bench/bench.h>
#include <aiz/dyn/directml.h>

#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace aiz {
namespace {

class DirectMLBenchBase : public IBenchmark {
protected:
  bool checkAvailable(std::string& err) const {
    return ::aiz::dyn::directml::isAvailable(&err);
  }

  double benchMatMul(int size, int iterations, std::string& /*err*/) const {
    // DirectML MatMul simulation
    // In a real implementation, this would:
    // 1. Create D3D12 device
    // 2. Create DirectML device
    // 3. Create MatMul operator
    // 4. Execute and measure

    // For now, simulate computation
    const size_t tensor_size = size * size;
    std::vector<float> a(tensor_size, 1.0f);
    std::vector<float> b(tensor_size, 1.0f);
    std::vector<float> c(tensor_size, 0.0f);

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
      // Naive CPU MatMul as placeholder
      for (int row = 0; row < size; ++row) {
        for (int col = 0; col < size; ++col) {
          float sum = 0.0f;
          for (int k = 0; k < size; ++k) {
            sum += a[row * size + k] * b[k * size + col];
          }
          c[row * size + col] = sum;
        }
      }
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    const double ops_per_matmul = 2.0 * size * size * size;
    const double total_ops = ops_per_matmul * iterations;
    const double gflops = (total_ops / elapsed_sec) / 1e9;

    return gflops;
  }

  double benchMemoryBandwidth(size_t bytes, int iterations, std::string& /*err*/) const {
    const size_t num_floats = bytes / sizeof(float);
    std::vector<float> data(num_floats, 1.0f);

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
      float sum = 0.0f;
      for (size_t j = 0; j < num_floats; ++j) {
        sum += data[j];
      }
      if (sum < 0.0f) break;
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    const double total_bytes = bytes * iterations;
    const double gb_per_sec = (total_bytes / elapsed_sec) / 1e9;

    return gb_per_sec;
  }
};

class DirectMLMatMulBenchmark final : public DirectMLBenchBase {
public:
  std::string name() const override { return "DirectML FP32 MatMul"; }

  bool isAvailable() const override {
    std::string err;
    return checkAvailable(err);
  }

  BenchResult run() override {
    std::string err;
    if (!checkAvailable(err)) {
      return BenchResult{false, err};
    }

    const double gflops = benchMatMul(512, 10, err);
    if (gflops < 0.0) {
      return BenchResult{false, err};
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
  }
};

class DirectMLMemoryBandwidthBenchmark final : public DirectMLBenchBase {
public:
  std::string name() const override { return "DirectML Memory BW"; }

  bool isAvailable() const override {
    std::string err;
    return checkAvailable(err);
  }

  BenchResult run() override {
    std::string err;
    if (!checkAvailable(err)) {
      return BenchResult{false, err};
    }

    const double gb_per_sec = benchMemoryBandwidth(256 * 1024 * 1024, 10, err);
    if (gb_per_sec < 0.0) {
      return BenchResult{false, err};
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gb_per_sec << " GB/s";
    return BenchResult{true, oss.str()};
  }
};

}  // namespace

// Factory functions
std::unique_ptr<IBenchmark> makeDirectMLMatMulBenchmark() {
  return std::make_unique<DirectMLMatMulBenchmark>();
}

std::unique_ptr<IBenchmark> makeDirectMLMemoryBandwidthBenchmark() {
  return std::make_unique<DirectMLMemoryBandwidthBenchmark>();
}

}  // namespace aiz
