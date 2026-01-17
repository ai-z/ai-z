cmake_minimum_required(VERSION 3.16)
project(ai-z VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CUDA is enabled conditionally; keep its dialect compatible with older CMake.
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

option(AI_Z_ENABLE_NVML "Enable NVIDIA NVML backend" ON)
option(AI_Z_ENABLE_CUDA "Enable CUDA benchmarks/backends" ON)
option(AI_Z_ENABLE_OPENCL "Enable OpenCL benchmarks/backends" OFF)
option(AI_Z_ENABLE_VULKAN "Enable Vulkan benchmarks/backends" OFF)
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

include(CTest)

add_executable(ai-z
  src/main.cpp
  src/aiz/app.cpp
  src/aiz/config/config.cpp
  src/aiz/hw/hardware_info.cpp
  src/aiz/metrics/cpu_usage.cpp
  src/aiz/metrics/disk_bandwidth.cpp
  src/aiz/metrics/network_bandwidth.cpp
  src/aiz/metrics/linux_gpu_sysfs.cpp
  src/aiz/metrics/gpu_memory_util.cpp
  src/aiz/metrics/timeline.cpp
  src/aiz/metrics/nvidia_nvml.cpp
  src/aiz/metrics/gpu_stub.cpp
  src/aiz/metrics/pcie_stub.cpp
  src/aiz/metrics/ram_usage.cpp
  src/aiz/bench/bench_stub.cpp
  src/aiz/bench/pcie_bandwidth_cuda_pergpu.cpp
  src/aiz/bench/pcie_bandwidth_opencl.cpp
  src/aiz/bench/gpu_compute_opencl.cpp
  src/aiz/bench/pcie_bandwidth_vulkan.cpp
  src/aiz/bench/gpu_compute_vulkan.cpp
)

target_include_directories(ai-z PRIVATE include)
target_include_directories(ai-z PRIVATE ${AI_Z_GENERATED_DIR})

if (WIN32)
  target_sources(ai-z PRIVATE
    src/aiz/tui/windows_console_ui.cpp
    src/aiz/tui/win32_terminal_ui.cpp
    src/aiz/tui/tui_core.cpp
  )
  target_link_libraries(ai-z PRIVATE pdh iphlpapi)
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
    enable_language(CUDA)
    # Avoid empty CUDA_ARCHITECTURES warnings. "native" can map to compute_native,
    # which older NVCC versions don't accept; use a conservative default set.
    set_property(TARGET ai-z PROPERTY CUDA_ARCHITECTURES 70 75 80 86)
    target_sources(ai-z PRIVATE src/aiz/bench/pcie_bandwidth_cuda.cpp)
    target_sources(ai-z PRIVATE src/aiz/bench/gpu_compute_cuda.cu)
    target_link_libraries(ai-z PRIVATE CUDA::cudart)
  endif()
endif()

if (AI_Z_ENABLE_OPENCL)
  find_package(OpenCL QUIET)
  if (NOT OpenCL_FOUND)
    message(WARNING "AI_Z_ENABLE_OPENCL=ON but OpenCL not found; disabling OpenCL backend. Configure with -DAI_Z_ENABLE_OPENCL=OFF to silence.")
    set(AI_Z_ENABLE_OPENCL OFF CACHE BOOL "Enable OpenCL benchmarks/backends" FORCE)
  else()
    if (TARGET OpenCL::OpenCL)
      target_link_libraries(ai-z PRIVATE OpenCL::OpenCL)
    else()
      target_include_directories(ai-z PRIVATE ${OpenCL_INCLUDE_DIRS})
      target_link_libraries(ai-z PRIVATE ${OpenCL_LIBRARIES})
    endif()
  endif()
endif()

if (AI_Z_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if (NOT Vulkan_FOUND)
    message(WARNING "AI_Z_ENABLE_VULKAN=ON but Vulkan not found; disabling Vulkan backend. Configure with -DAI_Z_ENABLE_VULKAN=OFF to silence.")
    set(AI_Z_ENABLE_VULKAN OFF CACHE BOOL "Enable Vulkan benchmarks/backends" FORCE)
  else()
    if (TARGET Vulkan::Vulkan)
      target_link_libraries(ai-z PRIVATE Vulkan::Vulkan)
    else()
      target_include_directories(ai-z PRIVATE ${Vulkan_INCLUDE_DIRS})
      target_link_libraries(ai-z PRIVATE ${Vulkan_LIBRARIES})
    endif()

    # Optional: shaderc enables runtime GLSL->SPIR-V compilation for Vulkan compute benchmarks.
    # Install via distro package (e.g. libshaderc-dev) to enable.
    find_package(shaderc CONFIG QUIET)
    set(AI_Z_SHADERC_FOUND FALSE)
    if (shaderc_FOUND)
      set(AI_Z_SHADERC_FOUND TRUE)
      if (TARGET shaderc::shaderc)
        target_link_libraries(ai-z PRIVATE shaderc::shaderc)
      elseif (TARGET shaderc_shared)
        target_link_libraries(ai-z PRIVATE shaderc_shared)
      elseif (TARGET shaderc)
        target_link_libraries(ai-z PRIVATE shaderc)
      endif()
    else()
      # Many distros ship shaderc without a CMake config package; try pkg-config and plain find.
      find_package(PkgConfig QUIET)
      if (PkgConfig_FOUND)
        pkg_check_modules(SHADERC QUIET shaderc)
      endif()

      find_path(SHADERC_INCLUDE_DIR shaderc/shaderc.hpp
        HINTS ${SHADERC_INCLUDE_DIRS}
      )
      find_library(SHADERC_LIBRARY
        NAMES shaderc_shared shaderc_combined shaderc
        HINTS ${SHADERC_LIBRARY_DIRS}
      )
      if (SHADERC_INCLUDE_DIR AND SHADERC_LIBRARY)
        set(AI_Z_SHADERC_FOUND TRUE)
        target_include_directories(ai-z PRIVATE ${SHADERC_INCLUDE_DIR})
        target_link_libraries(ai-z PRIVATE ${SHADERC_LIBRARY})
      endif()
    endif()

    if (AI_Z_SHADERC_FOUND)
      target_compile_definitions(ai-z PRIVATE AI_Z_HAS_SHADERC)
    else()
      message(STATUS "shaderc not found; runtime GLSL->SPIR-V compilation disabled (Vulkan FP32 uses embedded SPIR-V).")
    endif()
  endif()
endif()

target_compile_definitions(ai-z PRIVATE
  $<$<BOOL:${AI_Z_ENABLE_NVML}>:AI_Z_ENABLE_NVML>
  $<$<BOOL:${AI_Z_ENABLE_CUDA}>:AI_Z_ENABLE_CUDA>
  $<$<BOOL:${AI_Z_ENABLE_OPENCL}>:AI_Z_ENABLE_OPENCL>
  $<$<BOOL:${AI_Z_ENABLE_VULKAN}>:AI_Z_ENABLE_VULKAN>
  $<$<BOOL:${AI_Z_ENABLE_HIP}>:AI_Z_ENABLE_HIP>
  $<$<BOOL:${AI_Z_ENABLE_LIBTORCH}>:AI_Z_ENABLE_LIBTORCH>
  $<$<BOOL:${AI_Z_ENABLE_LEVEL_ZERO}>:AI_Z_ENABLE_LEVEL_ZERO>
  $<$<BOOL:${AI_Z_ENABLE_SYCL}>:AI_Z_ENABLE_SYCL>
)

# Keep warnings reasonable for early scaffold.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ai-z PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (BUILD_TESTING)
  add_subdirectory(tests)
endif()
