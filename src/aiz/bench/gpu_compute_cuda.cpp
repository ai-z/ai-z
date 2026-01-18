#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_CUDA

#include <aiz/dyn/cuda.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace aiz {
namespace {

static const ::aiz::dyn::cuda::Api* cuApi(std::string& err) {
  return ::aiz::dyn::cuda::api(&err);
}

static BenchResult failCuda(const std::string& where, ::aiz::dyn::cuda::CUresult r) {
  return BenchResult{false, where + ": " + ::aiz::dyn::cuda::errToString(r)};
}

struct CudaCtx {
  const ::aiz::dyn::cuda::Api* cu = nullptr;
  ::aiz::dyn::cuda::CUcontext ctx = nullptr;

  CudaCtx() = default;
  CudaCtx(const CudaCtx&) = delete;
  CudaCtx& operator=(const CudaCtx&) = delete;

  CudaCtx(CudaCtx&& other) noexcept : cu(other.cu), ctx(other.ctx) {
    other.cu = nullptr;
    other.ctx = nullptr;
  }

  CudaCtx& operator=(CudaCtx&& other) noexcept {
    if (this != &other) {
      if (cu && ctx) {
        (void)cu->cuCtxDestroy_v2(ctx);
      }
      cu = other.cu;
      ctx = other.ctx;
      other.cu = nullptr;
      other.ctx = nullptr;
    }
    return *this;
  }

  ~CudaCtx() {
    if (cu && ctx) {
      (void)cu->cuCtxDestroy_v2(ctx);
    }
  }
};

static std::optional<CudaCtx> createContextForGpu(unsigned int gpuIndex, int& deviceCount, std::string& err) {
  const auto* cu = cuApi(err);
  if (!cu) return std::nullopt;

  deviceCount = 0;
  int n = 0;
  ::aiz::dyn::cuda::CUresult r = cu->cuDeviceGetCount(&n);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuDeviceGetCount failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }
  deviceCount = n;
  if (n <= 0) {
    err = "No CUDA devices found.";
    return std::nullopt;
  }
  if (gpuIndex >= static_cast<unsigned int>(n)) {
    err = "Invalid GPU index.";
    return std::nullopt;
  }

  ::aiz::dyn::cuda::CUdevice dev = 0;
  r = cu->cuDeviceGet(&dev, static_cast<int>(gpuIndex));
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuDeviceGet failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  CudaCtx out;
  out.cu = cu;
  r = cu->cuCtxCreate_v2(&out.ctx, 0, dev);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuCtxCreate_v2 failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  return std::optional<CudaCtx>{std::move(out)};
}

// A tiny PTX kernel that runs a simple FP32 FMA-style loop.
// This is intentionally conservative so it can JIT on many driver versions.
static const char* kPtxFp32 = R"PTX(
.version 6.0
.target sm_30
.address_size 64

.visible .entry fma_fp32(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<6>;
  .reg .b64 %rd<6>;
  .reg .f32 %f<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  cvt.rn.f32.s32 %f1, %r5;
  mov.f32 %f2, 1.0000001;
  mov.f32 %f3, 0.0000001;

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;
  fma.rn.f32 %f1, %f1, %f2, %f3;
  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  mul.wide.s32 %rd2, %r5, 4;
  add.s64 %rd3, %rd1, %rd2;
  st.global.f32 [%rd3], %f1;
  ret;
}
)PTX";

static const char* kPtxFp64 = R"PTX(
.version 6.0
.target sm_30
.address_size 64

.visible .entry fma_fp64(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<6>;
  .reg .b64 %rd<6>;
  .reg .f64 %d<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  cvt.rn.f64.s32 %d1, %r5;
  mov.f64 %d2, 1.0000001;
  mov.f64 %d3, 0.0000001;

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;
  fma.rn.f64 %d1, %d1, %d2, %d3;
  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  mul.wide.s32 %rd2, %r5, 8;
  add.s64 %rd3, %rd1, %rd2;
  st.global.f64 [%rd3], %d1;
  ret;
}
)PTX";

// FP16 compute.
// We first try a native FP16 FMA kernel that requires hardware/driver support.
// If that fails to load/JIT, we fall back to an "emulated" FP16 kernel that
// performs FP32 FMA but rounds to FP16 every iteration.
static const char* kPtxFp16Native = R"PTX(
.version 6.0
.target sm_53
.address_size 64

.visible .entry fma_fp16(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<6>;
  .reg .b64 %rd<6>;
  .reg .f32 %f<6>;
  .reg .b16 %h<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  // Seed values.
  cvt.rn.f32.s32 %f1, %r5;
  mov.f32 %f2, 0.0009765625; // 1/1024
  mul.rn.f32 %f1, %f1, %f2;
  cvt.rn.f16.f32 %h1, %f1;

  mov.f32 %f3, 1.0009766;
  cvt.rn.f16.f32 %h2, %f3;
  mov.f32 %f4, 0.0009766;
  cvt.rn.f16.f32 %h3, %f4;

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;
  // one FMA in FP16
  fma.rn.f16 %h1, %h1, %h2, %h3;
  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  cvt.f32.f16 %f5, %h1;
  mul.wide.s32 %rd2, %r5, 4;
  add.s64 %rd3, %rd1, %rd2;
  st.global.f32 [%rd3], %f5;
  ret;
}
)PTX";

static const char* kPtxFp16Emu = R"PTX(
.version 6.0
.target sm_30
.address_size 64

.visible .entry fma_fp16_emu(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<6>;
  .reg .b64 %rd<6>;
  .reg .f32 %f<8>;
  .reg .b16 %h<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  // Seed values.
  cvt.rn.f32.s32 %f1, %r5;
  mov.f32 %f2, 0.0009765625; // 1/1024
  mul.rn.f32 %f1, %f1, %f2;
  cvt.rn.f16.f32 %h1, %f1;

  mov.f32 %f3, 1.0009766;
  cvt.rn.f16.f32 %h2, %f3;
  mov.f32 %f4, 0.0009766;
  cvt.rn.f16.f32 %h3, %f4;

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;
  // FP32 FMA but round-trip through FP16 each iteration.
  cvt.f32.f16 %f5, %h1;
  cvt.f32.f16 %f6, %h2;
  cvt.f32.f16 %f7, %h3;
  fma.rn.f32 %f5, %f5, %f6, %f7;
  cvt.rn.f16.f32 %h1, %f5;
  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  cvt.f32.f16 %f5, %h1;
  mul.wide.s32 %rd2, %r5, 4;
  add.s64 %rd3, %rd1, %rd2;
  st.global.f32 [%rd3], %f5;
  ret;
}
)PTX";

static const char* kPtxInt32 = R"PTX(
.version 6.0
.target sm_30
.address_size 64

.visible .entry mad_int32(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<10>;
  .reg .b64 %rd<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  // a = (tid % 97) + 1, b = (tid % 89) + 2
  rem.s32 %r6, %r5, 97;
  add.s32 %r6, %r6, 1;
  rem.s32 %r7, %r5, 89;
  add.s32 %r7, %r7, 2;
  mov.s32 %r8, 1;

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;
  mad.lo.s32 %r6, %r6, %r7, %r8;
  mad.lo.s32 %r7, %r7, %r6, %r8;
  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  add.s32 %r9, %r6, %r7;
  mul.wide.s32 %rd2, %r5, 4;
  add.s64 %rd3, %rd1, %rd2;
  st.global.s32 [%rd3], %r9;
  ret;
}
)PTX";

static const char* kPtxInt4 = R"PTX(
.version 6.0
.target sm_30
.address_size 64

.visible .entry mad_int4(
  .param .u64 out_ptr,
  .param .u32 iters
)
{
  .reg .pred %p;
  .reg .b32 %r<40>;
  .reg .b64 %rd<6>;

  ld.param.u64 %rd1, [out_ptr];
  ld.param.u32 %r1, [iters];

  mov.u32 %r2, %tid.x;
  mov.u32 %r3, %ctaid.x;
  mov.u32 %r4, %ntid.x;
  mad.lo.s32 %r5, %r3, %r4, %r2;

  // aPack = 0x1234abcd ^ (tid * 2654435761)
  // bPack = 0x0f1e2d3c ^ (tid * 2246822519)
  mul.lo.u32 %r6, %r5, 2654435761;
  xor.b32 %r7, %r6, 0x1234abcd;
  mul.lo.u32 %r8, %r5, 2246822519;
  xor.b32 %r9, %r8, 0x0f1e2d3c;

  mov.s32 %r10, 1; // acc

LOOP:
  setp.le.s32 %p, %r1, 0;
  @%p bra DONE;

  // lane 0
  and.b32 %r11, %r7, 0xF;
  cvt.s32.u32 %r11, %r11;
  add.s32 %r11, %r11, -8;
  and.b32 %r12, %r9, 0xF;
  cvt.s32.u32 %r12, %r12;
  add.s32 %r12, %r12, -8;
  mad.lo.s32 %r10, %r11, %r12, %r10;

  // lane 1
  shr.u32 %r13, %r7, 4;
  and.b32 %r13, %r13, 0xF;
  cvt.s32.u32 %r13, %r13;
  add.s32 %r13, %r13, -8;
  shr.u32 %r14, %r9, 4;
  and.b32 %r14, %r14, 0xF;
  cvt.s32.u32 %r14, %r14;
  add.s32 %r14, %r14, -8;
  mad.lo.s32 %r10, %r13, %r14, %r10;

  // lane 2
  shr.u32 %r15, %r7, 8;
  and.b32 %r15, %r15, 0xF;
  cvt.s32.u32 %r15, %r15;
  add.s32 %r15, %r15, -8;
  shr.u32 %r16, %r9, 8;
  and.b32 %r16, %r16, 0xF;
  cvt.s32.u32 %r16, %r16;
  add.s32 %r16, %r16, -8;
  mad.lo.s32 %r10, %r15, %r16, %r10;

  // lane 3
  shr.u32 %r17, %r7, 12;
  and.b32 %r17, %r17, 0xF;
  cvt.s32.u32 %r17, %r17;
  add.s32 %r17, %r17, -8;
  shr.u32 %r18, %r9, 12;
  and.b32 %r18, %r18, 0xF;
  cvt.s32.u32 %r18, %r18;
  add.s32 %r18, %r18, -8;
  mad.lo.s32 %r10, %r17, %r18, %r10;

  // lane 4
  shr.u32 %r19, %r7, 16;
  and.b32 %r19, %r19, 0xF;
  cvt.s32.u32 %r19, %r19;
  add.s32 %r19, %r19, -8;
  shr.u32 %r20, %r9, 16;
  and.b32 %r20, %r20, 0xF;
  cvt.s32.u32 %r20, %r20;
  add.s32 %r20, %r20, -8;
  mad.lo.s32 %r10, %r19, %r20, %r10;

  // lane 5
  shr.u32 %r21, %r7, 20;
  and.b32 %r21, %r21, 0xF;
  cvt.s32.u32 %r21, %r21;
  add.s32 %r21, %r21, -8;
  shr.u32 %r22, %r9, 20;
  and.b32 %r22, %r22, 0xF;
  cvt.s32.u32 %r22, %r22;
  add.s32 %r22, %r22, -8;
  mad.lo.s32 %r10, %r21, %r22, %r10;

  // lane 6
  shr.u32 %r23, %r7, 24;
  and.b32 %r23, %r23, 0xF;
  cvt.s32.u32 %r23, %r23;
  add.s32 %r23, %r23, -8;
  shr.u32 %r24, %r9, 24;
  and.b32 %r24, %r24, 0xF;
  cvt.s32.u32 %r24, %r24;
  add.s32 %r24, %r24, -8;
  mad.lo.s32 %r10, %r23, %r24, %r10;

  // lane 7
  shr.u32 %r25, %r7, 28;
  and.b32 %r25, %r25, 0xF;
  cvt.s32.u32 %r25, %r25;
  add.s32 %r25, %r25, -8;
  shr.u32 %r26, %r9, 28;
  and.b32 %r26, %r26, 0xF;
  cvt.s32.u32 %r26, %r26;
  add.s32 %r26, %r26, -8;
  mad.lo.s32 %r10, %r25, %r26, %r10;

  // Rotate packs: a=(a<<1)|(a>>31), b=(b<<3)|(b>>29)
  shl.b32 %r27, %r7, 1;
  shr.u32 %r28, %r7, 31;
  or.b32 %r7, %r27, %r28;

  shl.b32 %r29, %r9, 3;
  shr.u32 %r30, %r9, 29;
  or.b32 %r9, %r29, %r30;

  add.s32 %r1, %r1, -1;
  bra LOOP;

DONE:
  mul.wide.s32 %rd2, %r5, 4;
  add.s64 %rd3, %rd1, %rd2;
  st.global.s32 [%rd3], %r10;
  ret;
}
)PTX";

struct ModuleAndFunction {
  ::aiz::dyn::cuda::CUmodule mod = nullptr;
  ::aiz::dyn::cuda::CUfunction fn = nullptr;
};

static std::optional<ModuleAndFunction> loadKernel(const ::aiz::dyn::cuda::Api* cu,
                                                   const char* ptx,
                                                   const char* kernelName,
                                                   std::string& err) {
  ModuleAndFunction mf;
  char jitErrLog[8192] = {0};
  unsigned int jitErrLogSize = static_cast<unsigned int>(sizeof(jitErrLog));
  ::aiz::dyn::cuda::CUjit_option opts[] = {
      ::aiz::dyn::cuda::CU_JIT_ERROR_LOG_BUFFER,
      ::aiz::dyn::cuda::CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
  };
  void* optVals[] = {
      jitErrLog,
      &jitErrLogSize,
  };

  ::aiz::dyn::cuda::CUresult r = cu->cuModuleLoadDataEx(&mf.mod, ptx, 2, opts, optVals);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuModuleLoadDataEx failed: " + ::aiz::dyn::cuda::errToString(r);
    if (jitErrLog[0] != '\0') {
      err += "; JIT log: ";
      err += jitErrLog;
    }
    return std::nullopt;
  }

  r = cu->cuModuleGetFunction(&mf.fn, mf.mod, kernelName);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    (void)cu->cuModuleUnload(mf.mod);
    err = std::string("cuModuleGetFunction(") + kernelName + ") failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  return mf;
}

static void unloadKernel(const ::aiz::dyn::cuda::Api* cu, ModuleAndFunction& mf) {
  if (mf.mod) (void)cu->cuModuleUnload(mf.mod);
  mf.mod = nullptr;
  mf.fn = nullptr;
}

static std::optional<double> runKernelGops(const ::aiz::dyn::cuda::Api* cu,
                                          ::aiz::dyn::cuda::CUfunction fn,
                                          ::aiz::dyn::cuda::CUdeviceptr outDev,
                                          std::size_t elemCount,
                                          int iters,
                                          double opsPerIterPerThread,
                                          std::string& err) {
  constexpr unsigned int threads = 256;
  const unsigned int blocks = static_cast<unsigned int>((elemCount + threads - 1) / threads);

  ::aiz::dyn::cuda::CUstream stream{};
  ::aiz::dyn::cuda::CUresult r = cu->cuStreamCreate(&stream, ::aiz::dyn::cuda::CU_STREAM_NON_BLOCKING);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    err = "cuStreamCreate failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  ::aiz::dyn::cuda::CUevent evStart{};
  ::aiz::dyn::cuda::CUevent evStop{};
  r = cu->cuEventCreate(&evStart, ::aiz::dyn::cuda::CU_EVENT_DEFAULT);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    (void)cu->cuStreamDestroy_v2(stream);
    err = "cuEventCreate(start) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }
  r = cu->cuEventCreate(&evStop, ::aiz::dyn::cuda::CU_EVENT_DEFAULT);
  if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
    (void)cu->cuEventDestroy_v2(evStart);
    (void)cu->cuStreamDestroy_v2(stream);
    err = "cuEventCreate(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
    return std::nullopt;
  }

  const std::uint32_t itersU32 = static_cast<std::uint32_t>(iters);

  // Warmup.
  {
    void* args[] = {&outDev, const_cast<std::uint32_t*>(&itersU32)};
    for (int i = 0; i < 2; ++i) {
      r = cu->cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, stream, args, nullptr);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        err = "cuLaunchKernel(warmup) failed: " + ::aiz::dyn::cuda::errToString(r);
        goto FAIL;
      }
    }
    r = cu->cuStreamSynchronize(stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = "cuStreamSynchronize(warmup) failed: " + ::aiz::dyn::cuda::errToString(r);
      goto FAIL;
    }
  }

  // Timed.
  {
    void* args[] = {&outDev, const_cast<std::uint32_t*>(&itersU32)};
    r = cu->cuEventRecord(evStart, stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = "cuEventRecord(start) failed: " + ::aiz::dyn::cuda::errToString(r);
      goto FAIL;
    }

    constexpr int timed = 5;
    for (int i = 0; i < timed; ++i) {
      r = cu->cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, stream, args, nullptr);
      if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
        err = "cuLaunchKernel(timed) failed: " + ::aiz::dyn::cuda::errToString(r);
        goto FAIL;
      }
    }

    r = cu->cuEventRecord(evStop, stream);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = "cuEventRecord(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
      goto FAIL;
    }

    r = cu->cuEventSynchronize(evStop);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = "cuEventSynchronize(stop) failed: " + ::aiz::dyn::cuda::errToString(r);
      goto FAIL;
    }

    float ms = 0.0f;
    r = cu->cuEventElapsedTime(&ms, evStart, evStop);
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) {
      err = "cuEventElapsedTime failed: " + ::aiz::dyn::cuda::errToString(r);
      goto FAIL;
    }

    (void)cu->cuEventDestroy_v2(evStart);
    (void)cu->cuEventDestroy_v2(evStop);
    (void)cu->cuStreamDestroy_v2(stream);

    const double sec = static_cast<double>(ms) / 1000.0;
    if (sec <= 0.0) {
      err = "timing failed";
      return std::nullopt;
    }

    const double totalOps = static_cast<double>(elemCount) * static_cast<double>(iters) * opsPerIterPerThread * 5.0;
    return (totalOps / sec) / 1e9;
  }

FAIL:
  (void)cu->cuEventDestroy_v2(evStart);
  (void)cu->cuEventDestroy_v2(evStop);
  (void)cu->cuStreamDestroy_v2(stream);
  return std::nullopt;
}

class GpuFp32Bench final : public IBenchmark {
public:
  explicit GpuFp32Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA FP32"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = cuApi(err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    int n = 0;
    auto ctx = createContextForGpu(gpuIndex_, n, err);
    if (!ctx) return BenchResult{false, err};

    const auto* cu = ctx->cu;

    constexpr std::size_t threads = 256;
    constexpr std::size_t blocks = 256;
    const std::size_t count = threads * blocks;

    ::aiz::dyn::cuda::CUdeviceptr outDev{};
    ::aiz::dyn::cuda::CUresult r = cu->cuMemAlloc_v2(&outDev, count * sizeof(float));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemAlloc_v2", r);

    ModuleAndFunction mf;
    auto mfOpt = loadKernel(cu, kPtxFp32, "fma_fp32", err);
    if (!mfOpt) {
      (void)cu->cuMemFree_v2(outDev);
      return BenchResult{false, err};
    }
    mf = *mfOpt;

    const int iters = 2048;
    const double opsPerIterPerThread = 2.0;  // one fma => 2 FLOPs
    const auto gops = runKernelGops(cu, mf.fn, outDev, count, iters, opsPerIterPerThread, err);

    unloadKernel(cu, mf);
    (void)cu->cuMemFree_v2(outDev);

    if (!gops) return BenchResult{false, err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gops << " GFLOPS";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

class GpuFp64Bench final : public IBenchmark {
public:
  explicit GpuFp64Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA FP64"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = cuApi(err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    int n = 0;
    auto ctx = createContextForGpu(gpuIndex_, n, err);
    if (!ctx) return BenchResult{false, err};

    const auto* cu = ctx->cu;

    constexpr std::size_t threads = 256;
    constexpr std::size_t blocks = 256;
    const std::size_t count = threads * blocks;

    ::aiz::dyn::cuda::CUdeviceptr outDev{};
    ::aiz::dyn::cuda::CUresult r = cu->cuMemAlloc_v2(&outDev, count * sizeof(double));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemAlloc_v2", r);

    auto mfOpt = loadKernel(cu, kPtxFp64, "fma_fp64", err);
    if (!mfOpt) {
      (void)cu->cuMemFree_v2(outDev);
      return BenchResult{false, err};
    }
    ModuleAndFunction mf = *mfOpt;

    const int iters = 1024;
    const double opsPerIterPerThread = 2.0;
    const auto gops = runKernelGops(cu, mf.fn, outDev, count, iters, opsPerIterPerThread, err);

    unloadKernel(cu, mf);
    (void)cu->cuMemFree_v2(outDev);

    if (!gops) return BenchResult{false, err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gops << " GFLOPS";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

class GpuInt8Bench final : public IBenchmark {
public:
  explicit GpuInt8Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA INT8"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = cuApi(err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    int n = 0;
    auto ctx = createContextForGpu(gpuIndex_, n, err);
    if (!ctx) return BenchResult{false, err};

    const auto* cu = ctx->cu;

    constexpr std::size_t threads = 256;
    constexpr std::size_t blocks = 256;
    const std::size_t count = threads * blocks;

    ::aiz::dyn::cuda::CUdeviceptr outDev{};
    ::aiz::dyn::cuda::CUresult r = cu->cuMemAlloc_v2(&outDev, count * sizeof(std::int32_t));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemAlloc_v2", r);

    auto mfOpt = loadKernel(cu, kPtxInt32, "mad_int32", err);
    if (!mfOpt) {
      (void)cu->cuMemFree_v2(outDev);
      return BenchResult{false, err};
    }
    ModuleAndFunction mf = *mfOpt;

    const int iters = 4096;
    const double opsPerIterPerThread = 4.0;  // two mad.lo.s32 operations => 4 ops (mul+add twice)
    const auto gops = runKernelGops(cu, mf.fn, outDev, count, iters, opsPerIterPerThread, err);

    unloadKernel(cu, mf);
    (void)cu->cuMemFree_v2(outDev);

    if (!gops) return BenchResult{false, err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gops << " GOPS";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

class GpuFp16Bench final : public IBenchmark {
public:
  explicit GpuFp16Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA FP16"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = cuApi(err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    int n = 0;
    auto ctx = createContextForGpu(gpuIndex_, n, err);
    if (!ctx) return BenchResult{false, err};

    const auto* cu = ctx->cu;

    constexpr std::size_t threads = 256;
    constexpr std::size_t blocks = 256;
    const std::size_t count = threads * blocks;

    ::aiz::dyn::cuda::CUdeviceptr outDev{};
    ::aiz::dyn::cuda::CUresult r = cu->cuMemAlloc_v2(&outDev, count * sizeof(float));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemAlloc_v2", r);

    bool native = true;
    auto mfOpt = loadKernel(cu, kPtxFp16Native, "fma_fp16", err);
    if (!mfOpt) {
      native = false;
      err.clear();
      mfOpt = loadKernel(cu, kPtxFp16Emu, "fma_fp16_emu", err);
    }
    if (!mfOpt) {
      (void)cu->cuMemFree_v2(outDev);
      return BenchResult{false, err};
    }
    ModuleAndFunction mf = *mfOpt;

    const int iters = 4096;
    const double opsPerIterPerThread = 2.0;  // one fma => 2 FLOPs
    const auto gops = runKernelGops(cu, mf.fn, outDev, count, iters, opsPerIterPerThread, err);

    unloadKernel(cu, mf);
    (void)cu->cuMemFree_v2(outDev);

    if (!gops) return BenchResult{false, err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gops << " GFLOPS";
    if (!native) out << " (emu)";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

class UnavailableCudaInt4 final : public IBenchmark {
public:
  explicit UnavailableCudaInt4(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA INT4"; }
  bool isAvailable() const override { return false; }
  BenchResult run() override {
    (void)gpuIndex_;
    return BenchResult{false, "CUDA INT4 benchmark is not available in the driver-only build."};
  }

private:
  unsigned int gpuIndex_ = 0;
};

class GpuInt4Bench final : public IBenchmark {
public:
  explicit GpuInt4Bench(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}
  std::string name() const override { return "CUDA INT4"; }

  bool isAvailable() const override {
    std::string err;
    const auto* cu = cuApi(err);
    if (!cu) return false;
    int n = 0;
    return (cu->cuDeviceGetCount(&n) == ::aiz::dyn::cuda::CUDA_SUCCESS) && (n > 0) && (gpuIndex_ < static_cast<unsigned int>(n));
  }

  BenchResult run() override {
    std::string err;
    int n = 0;
    auto ctx = createContextForGpu(gpuIndex_, n, err);
    if (!ctx) return BenchResult{false, err};

    const auto* cu = ctx->cu;

    constexpr std::size_t threads = 256;
    constexpr std::size_t blocks = 256;
    const std::size_t count = threads * blocks;

    ::aiz::dyn::cuda::CUdeviceptr outDev{};
    ::aiz::dyn::cuda::CUresult r = cu->cuMemAlloc_v2(&outDev, count * sizeof(std::int32_t));
    if (r != ::aiz::dyn::cuda::CUDA_SUCCESS) return failCuda("cuMemAlloc_v2", r);

    auto mfOpt = loadKernel(cu, kPtxInt4, "mad_int4", err);
    if (!mfOpt) {
      (void)cu->cuMemFree_v2(outDev);
      return BenchResult{false, err};
    }
    ModuleAndFunction mf = *mfOpt;

    const int iters = 2048;
    const double opsPerIterPerThread = 16.0;
    const auto gops = runKernelGops(cu, mf.fn, outDev, count, iters, opsPerIterPerThread, err);

    unloadKernel(cu, mf);
    (void)cu->cuMemFree_v2(outDev);

    if (!gops) return BenchResult{false, err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << *gops << " GOPS";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuFp16BenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<GpuFp16Bench>(gpuIndex);
}
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<GpuFp32Bench>(gpuIndex);
}
std::unique_ptr<IBenchmark> makeGpuFp64BenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<GpuFp64Bench>(gpuIndex);
}
std::unique_ptr<IBenchmark> makeGpuInt4BenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<GpuInt4Bench>(gpuIndex);
}
std::unique_ptr<IBenchmark> makeGpuInt8BenchmarkCuda(unsigned int gpuIndex) {
  return std::make_unique<GpuInt8Bench>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_CUDA
