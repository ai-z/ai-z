#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aiz::dyn::cuda {

// Minimal CUDA driver API surface.
// We intentionally avoid including CUDA headers so ai-z can build on systems
// without the CUDA toolkit installed.

using CUresult = int;
using CUdevice = int;
using CUcontext = struct CUctx_st*;
using CUmodule = struct CUmod_st*;
using CUfunction = struct CUfunc_st*;
using CUstream = struct CUstream_st*;
using CUevent = struct CUevent_st*;
using CUdeviceptr = std::uint64_t;

// Online compilation options (subset).
// Matches NVIDIA's CUjit_option enum values.
using CUjit_option = int;

// Common CUresult codes used for messaging.
constexpr CUresult CUDA_SUCCESS = 0;

// Flags we need (values match CUDA driver API; kept local to avoid headers).
constexpr unsigned int CU_EVENT_DEFAULT = 0x0;
constexpr unsigned int CU_EVENT_BLOCKING_SYNC = 0x1;
constexpr unsigned int CU_STREAM_DEFAULT = 0x0;
constexpr unsigned int CU_STREAM_NON_BLOCKING = 0x1;
constexpr unsigned int CU_MEMHOSTALLOC_PORTABLE = 0x1;

constexpr CUjit_option CU_JIT_MAX_REGISTERS = 0;
constexpr CUjit_option CU_JIT_THREADS_PER_BLOCK = 1;
constexpr CUjit_option CU_JIT_WALL_TIME = 2;
constexpr CUjit_option CU_JIT_INFO_LOG_BUFFER = 3;
constexpr CUjit_option CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES = 4;
constexpr CUjit_option CU_JIT_ERROR_LOG_BUFFER = 5;
constexpr CUjit_option CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6;

struct Api {
  void* handle = nullptr;

  CUresult (*cuInit)(unsigned int) = nullptr;
  CUresult (*cuDriverGetVersion)(int*) = nullptr;

  CUresult (*cuDeviceGetCount)(int*) = nullptr;
  CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
  CUresult (*cuDeviceGetName)(char*, int, CUdevice) = nullptr;

  CUresult (*cuCtxCreate_v2)(CUcontext*, unsigned int, CUdevice) = nullptr;
  CUresult (*cuCtxDestroy_v2)(CUcontext) = nullptr;

  CUresult (*cuStreamCreate)(CUstream*, unsigned int) = nullptr;
  CUresult (*cuStreamDestroy_v2)(CUstream) = nullptr;
  CUresult (*cuStreamSynchronize)(CUstream) = nullptr;

  CUresult (*cuMemAlloc_v2)(CUdeviceptr*, std::size_t) = nullptr;
  CUresult (*cuMemFree_v2)(CUdeviceptr) = nullptr;

  CUresult (*cuMemHostAlloc)(void**, std::size_t, unsigned int) = nullptr;
  CUresult (*cuMemFreeHost)(void*) = nullptr;

  CUresult (*cuMemcpyHtoDAsync_v2)(CUdeviceptr, const void*, std::size_t, CUstream) = nullptr;
  CUresult (*cuMemcpyDtoHAsync_v2)(void*, CUdeviceptr, std::size_t, CUstream) = nullptr;

  CUresult (*cuEventCreate)(CUevent*, unsigned int) = nullptr;
  CUresult (*cuEventDestroy_v2)(CUevent) = nullptr;
  CUresult (*cuEventRecord)(CUevent, CUstream) = nullptr;
  CUresult (*cuEventSynchronize)(CUevent) = nullptr;
  CUresult (*cuEventElapsedTime)(float*, CUevent, CUevent) = nullptr;

  CUresult (*cuModuleLoadDataEx)(CUmodule*, const void*, unsigned int, CUjit_option*, void**) = nullptr;
  CUresult (*cuModuleUnload)(CUmodule) = nullptr;
  CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;

  CUresult (*cuLaunchKernel)(
      CUfunction,
      unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
      unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
      unsigned int sharedMemBytes,
      CUstream hStream,
      void** kernelParams,
      void** extra) = nullptr;
};

// Returns nullptr if the CUDA driver runtime (libcuda) is not present or missing symbols.
// If errOut is non-null, it will be filled with a human-readable reason.
const Api* api(std::string* errOut);

// Best-effort error string. If the driver provides cuGetErrorString it will be used.
std::string errToString(CUresult r);

}  // namespace aiz::dyn::cuda
