#include <aiz/bench/bench.h>
#include <aiz/dyn/onnxruntime.h>
#include <aiz/metrics/npu_info.h>

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

// Base class for NPU benchmarks via ONNX Runtime
class OrtNpuBenchBase : public IBenchmark {
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

  // Configure session for Intel NPU via OpenVINO EP
  bool configureIntelNpuSession(const OrtApi* ort, OrtSessionOptions* opts, std::string& err) const {
    // Intel NPU is accessed through OpenVINO Execution Provider
    // The device target is specified via session options
    
    // Try to add OpenVINO EP with NPU device
    // ORT_API_STATUS(OrtApis::SessionOptionsAppendExecutionProvider_OpenVINO,
    //                _In_ OrtSessionOptions* options, _In_reads_(num_keys) const char* const* keys,
    //                _In_reads_(num_keys) const char* const* values, size_t num_keys);
    
    // For OpenVINO EP, we need to set device_type to "NPU"
    const char* keys[] = {"device_type", "precision"};
    const char* values[] = {"NPU", "FP16"};
    
    // Try to dynamically call the OpenVINO EP append function
    // This is done through the ORT API extensions mechanism
    
    // For now, we use a simpler approach - check if OpenVINO EP is available
    // and configure it for NPU
    
    // The actual implementation would need to:
    // 1. Load the OpenVINO EP shared library
    // 2. Get the provider options interface
    // 3. Configure for NPU device
    
    // Fallback: return false if we can't configure
    err = "OpenVINO EP NPU configuration not available in this build";
    return false;
  }

  // Configure session for AMD NPU via Vitis AI EP or XDNA EP
  bool configureAmdNpuSession(const OrtApi* ort, OrtSessionOptions* opts, std::string& err) const {
    // AMD NPU is accessed through Vitis AI EP or a dedicated NPU EP
    // Configuration depends on the available execution provider
    
    // For Vitis AI EP:
    // const char* keys[] = {"config_file", "target"};
    // const char* values[] = {"/path/to/config.json", "NPU"};
    
    err = "Vitis AI EP NPU configuration not available in this build";
    return false;
  }

  double benchMatMulNpu(NpuVendor vendor, int size, int iterations, std::string& err) const {
    if (size <= 0 || iterations <= 0) {
      err = "Invalid benchmark parameters.";
      return -1.0;
    }

    // Suppress stderr to prevent ORT library messages from corrupting TUI
    StderrSuppressor suppressStderr;

    const OrtApi* ort = ::aiz::dyn::onnxruntime::api(&err);
    if (!ort) return -1.0;

    // Embedded model bytes
    #include "onnxruntime_matmul_model.inc"

    OrtEnv* env = nullptr;
    {
      // Use ERROR level to suppress console output that causes TUI flickering
      OrtStatus* st = ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "ai-z-npu", &env);
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
      (void)ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    }

    // Configure NPU execution provider based on vendor
    bool npuConfigured = false;
    if (vendor == NpuVendor::Intel) {
      npuConfigured = configureIntelNpuSession(ort, opts, err);
    } else if (vendor == NpuVendor::AMD) {
      npuConfigured = configureAmdNpuSession(ort, opts, err);
    }

    if (!npuConfigured) {
      // NPU EP not available, clean up and return
      ort->ReleaseSessionOptions(opts);
      ort->ReleaseEnv(env);
      return -1.0;
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
};

// Intel NPU MatMul Benchmark
class IntelNpuMatMulBenchmark final : public OrtNpuBenchBase {
public:
  std::string name() const override { return "Intel NPU FP32 MatMul"; }

  bool isAvailable() const override {
    std::string err;
    if (!checkOrtLoaded(err)) return false;
    
    // Check if Intel NPU hardware is available
    auto npuResult = detail::probeIntelNpu();
    if (npuResult.status != NpuStatus::Available) return false;
    
    // Check if ORT OpenVINO EP is available
    auto ortNpu = probeOrtNpuProviders();
    return ortNpu.intelNpuAvailable;
  }

  BenchResult run() override {
    std::string err;
    if (!checkOrtLoaded(err)) {
      return BenchResult{false, err};
    }

#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
    constexpr int size = 512;
    constexpr int iters = 20;
    const double gflops = benchMatMulNpu(NpuVendor::Intel, size, iters, err);
    if (gflops < 0.0) return BenchResult{false, err};
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
#else
    return BenchResult{false,
                       "Intel NPU benchmark requires onnxruntime_c_api.h at build time. "
                       "Configure with -DAI_Z_ONNXRUNTIME_ROOT=/path/to/onnxruntime."};
#endif
  }
};

// AMD NPU MatMul Benchmark
class AmdNpuMatMulBenchmark final : public OrtNpuBenchBase {
public:
  std::string name() const override { return "AMD NPU FP32 MatMul"; }

  bool isAvailable() const override {
    std::string err;
    if (!checkOrtLoaded(err)) return false;
    
    // Check if AMD NPU hardware is available
    auto npuResult = detail::probeAmdNpu();
    if (npuResult.status != NpuStatus::Available) return false;
    
    // Check if ORT Vitis AI EP is available
    auto ortNpu = probeOrtNpuProviders();
    return ortNpu.amdNpuAvailable;
  }

  BenchResult run() override {
    std::string err;
    if (!checkOrtLoaded(err)) {
      return BenchResult{false, err};
    }

#if defined(AI_Z_HAS_ONNXRUNTIME_C_API)
    constexpr int size = 512;
    constexpr int iters = 20;
    const double gflops = benchMatMulNpu(NpuVendor::AMD, size, iters, err);
    if (gflops < 0.0) return BenchResult{false, err};
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
#else
    return BenchResult{false,
                       "AMD NPU benchmark requires onnxruntime_c_api.h at build time. "
                       "Configure with -DAI_Z_ONNXRUNTIME_ROOT=/path/to/onnxruntime."};
#endif
  }
};

// NPU Info Benchmark (reports detected NPU hardware and capabilities)
class NpuInfoBenchmark final : public IBenchmark {
public:
  std::string name() const override { return "NPU Detection"; }

  bool isAvailable() const override {
    // Always available - reports detection status
    return true;
  }

  BenchResult run() override {
    auto npuResult = probeNpuDevices();
    
    if (npuResult.status == NpuStatus::NoDevice) {
      return BenchResult{true, "No NPU detected"};
    }
    
    if (npuResult.status == NpuStatus::NoDriver) {
      return BenchResult{true, "NPU hardware found, driver not loaded"};
    }
    
    if (npuResult.devices.empty()) {
      return BenchResult{true, "NPU status: " + npuStatusToString(npuResult.status)};
    }
    
    std::ostringstream oss;
    oss << npuResult.devices.size() << " NPU(s): ";
    for (size_t i = 0; i < npuResult.devices.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << npuResult.devices[i].name;
      if (npuResult.devices[i].peakTops) {
        oss << " (" << static_cast<int>(*npuResult.devices[i].peakTops) << " TOPS)";
      }
    }
    
    return BenchResult{true, oss.str()};
  }
};

}  // namespace

// Factory functions
std::unique_ptr<IBenchmark> makeIntelNpuMatMulBenchmark() {
  return std::make_unique<IntelNpuMatMulBenchmark>();
}

std::unique_ptr<IBenchmark> makeAmdNpuMatMulBenchmark() {
  return std::make_unique<AmdNpuMatMulBenchmark>();
}

std::unique_ptr<IBenchmark> makeNpuInfoBenchmark() {
  return std::make_unique<NpuInfoBenchmark>();
}

}  // namespace aiz
