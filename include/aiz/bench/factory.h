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

std::unique_ptr<IBenchmark> makeGpuFp16Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp32Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuFp64Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt4Benchmark(unsigned int gpuIndex);
std::unique_ptr<IBenchmark> makeGpuInt8Benchmark(unsigned int gpuIndex);

std::unique_ptr<IBenchmark> makeCpuFp16FlopsBenchmark();
std::unique_ptr<IBenchmark> makeCpuFp32FlopsBenchmark();

}  // namespace aiz
