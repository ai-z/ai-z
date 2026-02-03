#include <aiz/bench/bench.h>
#include <aiz/dyn/onnxruntime.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Suppress stderr to prevent ORT library output from corrupting TUI
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
  #include <onnxruntime_c_api.h>
#endif

namespace aiz {
namespace {

// RAII helper to suppress stderr during ORT calls
class StderrSuppressor {
public:
  StderrSuppressor() {
#if defined(_WIN32)
    fflush(stderr);
    saved_stderr_ = _dup(_fileno(stderr));
    int null_fd = _open("NUL", _O_WRONLY);
    if (null_fd >= 0) {
      _dup2(null_fd, _fileno(stderr));
      _close(null_fd);
    }
#else
    fflush(stderr);
    saved_stderr_ = dup(STDERR_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
#endif
  }
  ~StderrSuppressor() {
    if (saved_stderr_ >= 0) {
#if defined(_WIN32)
      fflush(stderr);
      _dup2(saved_stderr_, _fileno(stderr));
      _close(saved_stderr_);
#else
      fflush(stderr);
      dup2(saved_stderr_, STDERR_FILENO);
      close(saved_stderr_);
#endif
    }
  }
private:
  int saved_stderr_ = -1;
};

class OrtBenchBase : public IBenchmark {
protected:
  bool checkOrtLoaded(std::string& err) const {
    return ::aiz::dyn::onnxruntime::api(&err) != nullptr;
  }

#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
  static std::string ortStatusToString(const OrtApi* ort, OrtStatus* status) {
    if (!status) return "";
    const char* msg = ort ? ort->GetErrorMessage(status) : nullptr;
    std::string out = msg ? msg : "(unknown ORT error)";
    if (ort) {
      ort->ReleaseStatus(status);
    }
    return out;
  }

  double benchMatMulCpuOrt(int size, int iterations, std::string& err) const {
    if (size <= 0 || iterations <= 0) {
      err = "Invalid benchmark parameters.";
      return -1.0;
    }

    // Suppress stderr to prevent ORT library messages from corrupting TUI
    StderrSuppressor suppressStderr;

    const OrtApi* ort = ::aiz::dyn::onnxruntime::api(&err);
    if (!ort) return -1.0;

    // Embedded model bytes.
    #include "onnxruntime_matmul_model.inc"

    OrtEnv* env = nullptr;
    {
      // Use ERROR level to suppress console output that causes TUI flickering
      OrtStatus* st = ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "ai-z", &env);
      if (st) {
        err = "ORT CreateEnv failed: " + ortStatusToString(ort, st);
        return -1.0;
      }
    }

    OrtSessionOptions* opts = nullptr;
    {
      OrtStatus* st = ort->CreateSessionOptions(&opts);
      if (st) {
        err = "ORT CreateSessionOptions failed: " + ortStatusToString(ort, st);
        ort->ReleaseEnv(env);
        return -1.0;
      }
      // Reasonable default optimization; leave threads at ORT defaults.
      (void)ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    }

    OrtSession* session = nullptr;
    {
      OrtStatus* st = ort->CreateSessionFromArray(env,
                                                  kAizOnnxMatMulDynamicModel,
                                                  static_cast<size_t>(kAizOnnxMatMulDynamicModelLen),
                                                  opts,
                                                  &session);
      if (st) {
        err = "ORT CreateSessionFromArray failed: " + ortStatusToString(ort, st);
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env);
        return -1.0;
      }
    }

    OrtMemoryInfo* mem = nullptr;
    {
      OrtStatus* st = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem);
      if (st) {
        err = "ORT CreateCpuMemoryInfo failed: " + ortStatusToString(ort, st);
        ort->ReleaseSession(session);
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env);
        return -1.0;
      }
    }

    const int64_t dimsA[2] = {static_cast<int64_t>(size), static_cast<int64_t>(size)};
    const int64_t dimsB[2] = {static_cast<int64_t>(size), static_cast<int64_t>(size)};
    const size_t elem_count = static_cast<size_t>(size) * static_cast<size_t>(size);

    std::vector<float> a(elem_count, 1.0f);
    std::vector<float> b(elem_count, 1.0f);

    OrtValue* a_val = nullptr;
    OrtValue* b_val = nullptr;
    {
      OrtStatus* st = ort->CreateTensorWithDataAsOrtValue(mem,
                                                          a.data(),
                                                          a.size() * sizeof(float),
                                                          dimsA,
                                                          2,
                                                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                          &a_val);
      if (st) {
        err = "ORT CreateTensor(A) failed: " + ortStatusToString(ort, st);
        ort->ReleaseMemoryInfo(mem);
        ort->ReleaseSession(session);
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env);
        return -1.0;
      }
      st = ort->CreateTensorWithDataAsOrtValue(mem,
                                               b.data(),
                                               b.size() * sizeof(float),
                                               dimsB,
                                               2,
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                               &b_val);
      if (st) {
        err = "ORT CreateTensor(B) failed: " + ortStatusToString(ort, st);
        ort->ReleaseValue(a_val);
        ort->ReleaseMemoryInfo(mem);
        ort->ReleaseSession(session);
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env);
        return -1.0;
      }
    }

    const char* input_names[] = {"A", "B"};
    OrtValue* input_vals[] = {a_val, b_val};
    const char* output_names[] = {"Y"};

    auto run_once = [&]() -> bool {
      OrtValue* out = nullptr;
      OrtStatus* st = ort->Run(session,
                               nullptr,
                               input_names,
                               input_vals,
                               2,
                               output_names,
                               1,
                               &out);
      if (st) {
        err = "ORT Run failed: " + ortStatusToString(ort, st);
        return false;
      }
      if (out) ort->ReleaseValue(out);
      return true;
    };

    // Warmup
    if (!run_once()) {
      ort->ReleaseValue(b_val);
      ort->ReleaseValue(a_val);
      ort->ReleaseMemoryInfo(mem);
      ort->ReleaseSession(session);
      ort->ReleaseSessionOptions(opts);
      ort->ReleaseEnv(env);
      return -1.0;
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      if (!run_once()) {
        ort->ReleaseValue(b_val);
        ort->ReleaseValue(a_val);
        ort->ReleaseMemoryInfo(mem);
        ort->ReleaseSession(session);
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env);
        return -1.0;
      }
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    ort->ReleaseValue(b_val);
    ort->ReleaseValue(a_val);
    ort->ReleaseMemoryInfo(mem);
    ort->ReleaseSession(session);
    ort->ReleaseSessionOptions(opts);
    ort->ReleaseEnv(env);

    const double ops_per_matmul = 2.0 * static_cast<double>(size) * static_cast<double>(size) * static_cast<double>(size);
    const double total_ops = ops_per_matmul * static_cast<double>(iterations);
    return (total_ops / elapsed_sec) / 1e9;
  }
#endif

  double benchMemoryBandwidth(bool use_cuda, size_t bytes, int iterations, std::string& err) const {
    if (use_cuda) {
      err = "CUDA provider benchmark not implemented";
      return -1.0;
    }

    // Allocate and fill memory
    const size_t num_floats = bytes / sizeof(float);
    std::vector<float> data(num_floats, 1.0f);

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
      // Simulate memory transfer
      float sum = 0.0f;
      for (size_t j = 0; j < num_floats; ++j) {
        sum += data[j];
      }
      // Use sum to prevent optimization
      if (sum < 0.0f) break;
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    const double total_bytes = bytes * iterations;
    const double gb_per_sec = (total_bytes / elapsed_sec) / 1e9;
    return gb_per_sec;
  }
};

class OrtCpuMatMulBenchmark final : public OrtBenchBase {
public:
  std::string name() const override { return "ONNX FP32 MatMul"; }

  bool isAvailable() const override {
    std::string err;
    return checkOrtLoaded(err);
  }

  BenchResult run() override {
    std::string err;
    if (!checkOrtLoaded(err)) {
      return BenchResult{false, err};
    }

#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
    constexpr int size = 512;
    constexpr int iters = 20;
    const double gflops = benchMatMulCpuOrt(size, iters, err);
    if (gflops < 0.0) return BenchResult{false, err};
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
#else
    return BenchResult{false,
                       "Real ORT execution requires onnxruntime_c_api.h at build time. "
                       "Configure with -DAI_Z_ONNXRUNTIME_ROOT=/path/to/onnxruntime (containing include/)."};
#endif
  }
};

class OrtCpuMemoryBandwidthBenchmark final : public OrtBenchBase {
public:
  std::string name() const override { return "ONNX Memory BW"; }

  bool isAvailable() const override {
    std::string err;
    return checkOrtLoaded(err);
  }

  BenchResult run() override {
    std::string err;
    if (!checkOrtLoaded(err)) {
      return BenchResult{false, err};
    }

    const double gb_per_sec = benchMemoryBandwidth(false, 256 * 1024 * 1024, 10, err);
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
std::unique_ptr<IBenchmark> makeOrtCpuMatMulBenchmark() {
  return std::make_unique<OrtCpuMatMulBenchmark>();
}

std::unique_ptr<IBenchmark> makeOrtCpuMemoryBandwidthBenchmark() {
  return std::make_unique<OrtCpuMemoryBandwidthBenchmark>();
}

}  // namespace aiz
