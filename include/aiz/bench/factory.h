#pragma once

#include <aiz/bench/bench.h>

#include <memory>

namespace aiz {

std::unique_ptr<IBenchmark> makePcieBandwidthBenchmark();
std::unique_ptr<IBenchmark> makeFlopsBenchmark();
std::unique_ptr<IBenchmark> makeTorchMatmulBenchmark();

// Per-device benchmarks used by the Benchmarks screen.
std::unique_ptr<IBenchmark> makeGpuPcieRxBenchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuPcieTxBenchmark(unsigned int gpuIndex);

// Per-device PCIe bandwidth tests (combined RX/TX), per backend.
std::unique_ptr<IBenchmark> makeGpuCudaPcieBandwidthBenchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuVulkanPcieBandwidthBenchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuOpenclPcieBandwidthBenchmark(unsigned int gpuIndex);

std::unique_ptr<IBenchmark> makeGpuFp16Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp64Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt4Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt8Benchmark(unsigned int gpuIndex);

// Per-device FP32 FLOPS tests, per backend.
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkVulkan(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkOpencl(unsigned int gpuIndex);

std::unique_ptr<IBenchmark> makeCpuFp16FlopsBenchmark();
std::unique_ptr<IBenchmark> makeCpuFp32FlopsBenchmark();

// ONNX Runtime benchmarks
std::unique_ptr<IBenchmark> makeOrtCpuMatMulBenchmark();
std::unique_ptr<IBenchmark> makeOrtCpuMemoryBandwidthBenchmark();

// NPU (Neural Processing Unit) benchmarks
std::unique_ptr<IBenchmark> makeIntelNpuMatMulBenchmark();
std::unique_ptr<IBenchmark> makeAmdNpuMatMulBenchmark();
std::unique_ptr<IBenchmark> makeNpuInfoBenchmark();

}  // namespace aiz
