#pragma once

#include <aiz/bench/bench.h>

#include <memory>

namespace aiz {

std::unique_ptr<IBenchmark> makePcieBandwidthBenchmark();
std::unique_ptr<IBenchmark> makeFlopsBenchmark();
std::unique_ptr<IBenchmark> makeTorchMatmulBenchmark();

}  // namespace aiz
