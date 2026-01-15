cmake_minimum_required(VERSION 3.16)
project(ai-z VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AI_Z_ENABLE_NVML "Enable NVIDIA NVML backend" ON)
option(AI_Z_ENABLE_CUDA "Enable CUDA benchmarks/backends" ON)
option(AI_Z_ENABLE_HIP "Enable HIP benchmarks/backends" OFF)
option(AI_Z_ENABLE_LIBTORCH "Enable LibTorch (PyTorch C++ API) benchmark" OFF)
option(AI_Z_ENABLE_LEVEL_ZERO "Enable Intel Level Zero backend" OFF)
option(AI_Z_ENABLE_SYCL "Enable SYCL/oneAPI backend" OFF)

set(AI_Z_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/aiz/version.h.in
  ${AI_Z_GENERATED_DIR}/aiz/version.h
  @ONLY
)

add_executable(ai-z
  src/main.cpp
  src/aiz/app.cpp
  src/aiz/config/config.cpp
  src/aiz/hw/hardware_info.cpp
  src/aiz/metrics/cpu_usage.cpp
  src/aiz/metrics/disk_bandwidth.cpp
  src/aiz/metrics/timeline.cpp
  src/aiz/metrics/nvidia_nvml.cpp
  src/aiz/metrics/gpu_stub.cpp
  src/aiz/metrics/pcie_stub.cpp
  src/aiz/metrics/ram_usage.cpp
  src/aiz/bench/bench_stub.cpp
)

target_include_directories(ai-z PRIVATE include)
target_include_directories(ai-z PRIVATE ${AI_Z_GENERATED_DIR})

if (WIN32)
  target_sources(ai-z PRIVATE
    src/aiz/tui/windows_console_ui.cpp
    src/aiz/tui/win32_terminal_ui.cpp
    src/aiz/tui/tui_core.cpp
  )
  target_link_libraries(ai-z PRIVATE pdh)
else()
  find_package(Curses REQUIRED)
  target_sources(ai-z PRIVATE
    src/aiz/tui/platform_ui_ncurses.cpp
    src/aiz/tui/ncurses_ui.cpp
    src/aiz/tui/tui_core.cpp
  )
  target_link_libraries(ai-z PRIVATE ${CURSES_LIBRARIES})
  target_include_directories(ai-z PRIVATE ${CURSES_INCLUDE_DIR})
endif()

if (UNIX)
  target_link_libraries(ai-z PRIVATE dl)
endif()

if (AI_Z_ENABLE_LIBTORCH)
  find_package(Torch REQUIRED)
  target_link_libraries(ai-z PRIVATE ${TORCH_LIBRARIES})
  target_compile_definitions(ai-z PRIVATE AI_Z_HAS_TORCH)
endif()

if (AI_Z_ENABLE_CUDA)
  find_package(CUDAToolkit QUIET)
  if (NOT CUDAToolkit_FOUND)
    message(WARNING "AI_Z_ENABLE_CUDA=ON but CUDAToolkit not found; disabling CUDA backend. Configure with -DAI_Z_ENABLE_CUDA=OFF to silence.")
    set(AI_Z_ENABLE_CUDA OFF CACHE BOOL "Enable CUDA benchmarks/backends" FORCE)
  else()
    target_sources(ai-z PRIVATE src/aiz/bench/pcie_bandwidth_cuda.cpp)
    target_link_libraries(ai-z PRIVATE CUDA::cudart)
  endif()
endif()

target_compile_definitions(ai-z PRIVATE
  $<$<BOOL:${AI_Z_ENABLE_NVML}>:AI_Z_ENABLE_NVML>
  $<$<BOOL:${AI_Z_ENABLE_CUDA}>:AI_Z_ENABLE_CUDA>
  $<$<BOOL:${AI_Z_ENABLE_HIP}>:AI_Z_ENABLE_HIP>
  $<$<BOOL:${AI_Z_ENABLE_LIBTORCH}>:AI_Z_ENABLE_LIBTORCH>
  $<$<BOOL:${AI_Z_ENABLE_LEVEL_ZERO}>:AI_Z_ENABLE_LEVEL_ZERO>
  $<$<BOOL:${AI_Z_ENABLE_SYCL}>:AI_Z_ENABLE_SYCL>
)

# Keep warnings reasonable for early scaffold.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ai-z PRIVATE -Wall -Wextra -Wpedantic)
endif()
