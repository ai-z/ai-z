cmake_minimum_required(VERSION 3.16)

# Versioning
#
# - Release builds should pass an explicit override (e.g. from a tag).
# - Developer builds can derive from git tags (vX.Y.Z) when available.
# - If neither is available (e.g. source archive without .git), fall back.
set(AI_Z_VERSION_FALLBACK "0.1.64")
set(AI_Z_VERSION_OVERRIDE "" CACHE STRING "Override ai-z version (e.g. 0.1.18)")
option(AI_Z_VERSION_FROM_GIT "Derive version from git tags (vX.Y.Z) when available" ON)

set(AI_Z_VERSION "${AI_Z_VERSION_FALLBACK}")
if (AI_Z_VERSION_OVERRIDE)
  set(AI_Z_VERSION "${AI_Z_VERSION_OVERRIDE}")
elseif (AI_Z_VERSION_FROM_GIT)
  find_package(Git QUIET)
  if (GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --abbrev=0
      WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
      OUTPUT_VARIABLE AI_Z_GIT_TAG
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if (AI_Z_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)")
      set(AI_Z_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()

project(ai-z VERSION ${AI_Z_VERSION} LANGUAGES C CXX)

# FTXUI is provided by vcpkg - no special Windows setup needed

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform check
if (NOT CMAKE_SYSTEM_NAME MATCHES "Linux|Windows")
  message(FATAL_ERROR "ai-z supports Linux and Windows only")
endif()

# Platform detection
set(AI_Z_PLATFORM_LINUX OFF)
set(AI_Z_PLATFORM_WINDOWS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(AI_Z_PLATFORM_LINUX ON)
elseif (WIN32)
  set(AI_Z_PLATFORM_WINDOWS ON)
endif()

# Backend defaults
#
# These backends are runtime-loaded, but building the benchmarks requires headers.
# We auto-enable them when headers are available to improve the out-of-box
# experience on systems with the relevant dev packages installed.
set(AI_Z_ENABLE_OPENCL_DEFAULT OFF)
find_package(OpenCL QUIET)
if (OpenCL_FOUND)
  set(AI_Z_ENABLE_OPENCL_DEFAULT ON)
endif()

set(AI_Z_ENABLE_VULKAN_DEFAULT OFF)
find_package(Vulkan QUIET)
if (Vulkan_FOUND)
  set(AI_Z_ENABLE_VULKAN_DEFAULT ON)
endif()

option(AI_Z_ENABLE_NVML "Enable NVIDIA NVML backend" ON)
option(AI_Z_ENABLE_CUDA "Enable CUDA benchmarks/backends" ON)
option(AI_Z_ENABLE_OPENCL "Enable OpenCL benchmarks/backends" ${AI_Z_ENABLE_OPENCL_DEFAULT})
option(AI_Z_ENABLE_VULKAN "Enable Vulkan benchmarks/backends" ${AI_Z_ENABLE_VULKAN_DEFAULT})
option(AI_Z_ENABLE_HIP "Enable HIP benchmarks/backends" OFF)
option(AI_Z_ENABLE_LIBTORCH "Enable LibTorch (PyTorch C++ API) benchmark" OFF)
option(AI_Z_ENABLE_LEVEL_ZERO "Enable Intel Level Zero backend" OFF)
option(AI_Z_ENABLE_SYCL "Enable SYCL/oneAPI backend" OFF)
option(AI_Z_ENABLE_WARNINGS "Enable -Wall/-Wextra style warnings" OFF)
option(AI_Z_BUILD_CLI "Build the ai-z CLI executable (requires FTXUI)" ON)

# Optional: ONNX Runtime headers.
# ai-z runtime-loads libonnxruntime via dlopen(), but a real ORT model-execution
# benchmark needs the C API header to safely call the function table.
set(AI_Z_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime root (contains include/ and lib/) for enabling real ORT benchmarks")

# Optional: AMD ADLX headers (official AMD SDK).
# ai-z runtime-loads amdadlx64.dll, but compiling the interface calls needs headers.
set(AI_Z_ADLX_ROOT "" CACHE PATH "ADLX SDK root (contains SDK/Include) for enabling ADLX integration")

# Optional: Intel IGCL headers + C API wrapper sources.
# IGCL runtime DLL is shipped with Intel graphics drivers on supported systems.
# We keep this optional so builds work without Intel SDK bits.
set(AI_Z_IGCL_ROOT "" CACHE PATH "IGCL SDK root (contains igcl_api.h and cApiWrapper.cpp) for enabling Intel telemetry")

set(AI_Z_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/aiz/version.h.in
  ${AI_Z_GENERATED_DIR}/aiz/version.h
  @ONLY
)

include(CTest)

# Core library sources (shared between CLI app and Python bindings)
# Excludes main.cpp and TUI sources which are app-specific
set(AI_Z_CORE_SOURCES
  src/aiz/app.cpp
  src/aiz/i18n.cpp
  src/aiz/config/config.cpp
  src/aiz/dyn/cuda.cpp
  src/aiz/dyn/opencl.cpp
  src/aiz/dyn/vulkan.cpp
  src/aiz/dyn/onnxruntime.cpp
  src/aiz/metrics/cpu_usage.cpp
  src/aiz/metrics/disk_bandwidth.cpp
  src/aiz/metrics/network_bandwidth.cpp
  src/aiz/metrics/gpu_memory_util.cpp
  src/aiz/metrics/timeline.cpp
  src/aiz/metrics/nvidia_nvml.cpp
  src/aiz/metrics/amd_adlx.cpp
  src/aiz/metrics/intel_igcl.cpp
  src/aiz/metrics/gpu_stub.cpp
  src/aiz/metrics/pcie_stub.cpp
  src/aiz/metrics/ram_usage.cpp
  src/aiz/metrics/process_list.cpp
  src/aiz/metrics/npu_info.cpp
  src/aiz/bench/bench_stub.cpp
  src/aiz/bench/report.cpp
  src/aiz/bench/pcie_bandwidth_cuda_pergpu.cpp
  src/aiz/bench/pcie_bandwidth_opencl.cpp
  src/aiz/bench/gpu_compute_opencl.cpp
  src/aiz/bench/pcie_bandwidth_vulkan.cpp
  src/aiz/bench/gpu_compute_vulkan.cpp
  src/aiz/bench/onnxruntime_bench.cpp
  src/aiz/bench/npu_bench.cpp
  src/aiz/metrics/gpu_sampler.cpp
  src/aiz/snapshot/snapshot.cpp
  src/aiz/snapshot/json_writer.cpp
)

if (AI_Z_PLATFORM_LINUX)
  set(AI_Z_PLATFORM_SOURCES
    src/aiz/platform/linux/dynlib_linux.cpp
    src/aiz/platform/linux/config_paths_linux.cpp
    src/aiz/platform/linux/process_linux.cpp
    src/aiz/platform/linux/metrics/cpu_linux.cpp
    src/aiz/platform/linux/metrics/memory_linux.cpp
    src/aiz/platform/linux/metrics/disk_linux.cpp
    src/aiz/platform/linux/metrics/network_linux.cpp
    src/aiz/metrics/amd_rocm_smi.cpp
    src/aiz/metrics/linux_gpu_sysfs.cpp
    src/aiz/metrics/npu_intel_linux.cpp
    src/aiz/metrics/npu_amd_linux.cpp
    src/aiz/hw/hardware_info.cpp
  )
elseif (AI_Z_PLATFORM_WINDOWS)
  set(AI_Z_PLATFORM_SOURCES
    src/aiz/platform/windows/dynlib_windows.cpp
    src/aiz/platform/windows/config_paths_windows.cpp
    src/aiz/platform/windows/process_windows.cpp
    src/aiz/platform/windows/metrics/cpu_windows.cpp
    src/aiz/platform/windows/metrics/memory_windows.cpp
    src/aiz/platform/windows/metrics/disk_windows.cpp
    src/aiz/platform/windows/metrics/network_windows.cpp
    src/aiz/metrics/windows_d3dkmt.cpp
    src/aiz/metrics/npu_intel_windows.cpp
    src/aiz/metrics/npu_amd_windows.cpp
    src/aiz/metrics/linux_gpu_sysfs.cpp
    src/aiz/metrics/amd_rocm_smi_stub.cpp
    src/aiz/hw/hardware_info_windows.cpp
  )
endif()

# Build the core metrics library (shared with Python bindings)
add_library(aiz-core STATIC ${AI_Z_CORE_SOURCES} ${AI_Z_PLATFORM_SOURCES})
set_target_properties(aiz-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(aiz-core PUBLIC include)
target_include_directories(aiz-core PUBLIC ${AI_Z_GENERATED_DIR})

# Platform compile definitions for core library
target_compile_definitions(aiz-core PUBLIC
  $<$<BOOL:${AI_Z_PLATFORM_LINUX}>:AI_Z_PLATFORM_LINUX>
  $<$<BOOL:${AI_Z_PLATFORM_WINDOWS}>:AI_Z_PLATFORM_WINDOWS>
)

target_compile_definitions(aiz-core PUBLIC
  $<$<BOOL:${AI_Z_ENABLE_NVML}>:AI_Z_ENABLE_NVML>
  $<$<BOOL:${AI_Z_ENABLE_CUDA}>:AI_Z_ENABLE_CUDA>
  $<$<BOOL:${AI_Z_ENABLE_OPENCL}>:AI_Z_ENABLE_OPENCL>
  $<$<BOOL:${AI_Z_ENABLE_VULKAN}>:AI_Z_ENABLE_VULKAN>
  $<$<BOOL:${AI_Z_ENABLE_HIP}>:AI_Z_ENABLE_HIP>
  $<$<BOOL:${AI_Z_ENABLE_LIBTORCH}>:AI_Z_ENABLE_LIBTORCH>
  $<$<BOOL:${AI_Z_ENABLE_LEVEL_ZERO}>:AI_Z_ENABLE_LEVEL_ZERO>
  $<$<BOOL:${AI_Z_ENABLE_SYCL}>:AI_Z_ENABLE_SYCL>
)

if (AI_Z_PLATFORM_LINUX)
  target_link_libraries(aiz-core PUBLIC dl)
elseif (AI_Z_PLATFORM_WINDOWS)
  target_link_libraries(aiz-core PUBLIC dxgi pdh setupapi iphlpapi psapi advapi32 shell32 ole32)
endif()

# Build the CLI executable (optional - requires FTXUI)
if (AI_Z_BUILD_CLI)
  add_executable(ai-z src/main.cpp)
  target_link_libraries(ai-z PRIVATE aiz-core)

  # Windows: embed application icon and version info
  if (AI_Z_PLATFORM_WINDOWS)
    set(AI_Z_RC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/ai-z.rc")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/ai-z.ico")
      target_sources(ai-z PRIVATE ${AI_Z_RC_FILE})
    endif()
  endif()
endif()

if (AI_Z_PLATFORM_WINDOWS)
  set(AI_Z_ADLX_INCLUDE_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../ADLX/SDK/Include")
  if (AI_Z_ADLX_ROOT)
    list(APPEND AI_Z_ADLX_INCLUDE_HINTS "${AI_Z_ADLX_ROOT}/SDK/Include")
    list(APPEND AI_Z_ADLX_INCLUDE_HINTS "${AI_Z_ADLX_ROOT}/Include")
  endif()

  find_path(AI_Z_ADLX_INCLUDE_DIR
    NAMES ADLX.h ISystem1.h ISystem2.h
    HINTS ${AI_Z_ADLX_INCLUDE_HINTS}
  )

  if (AI_Z_ADLX_INCLUDE_DIR)
    target_include_directories(aiz-core PUBLIC ${AI_Z_ADLX_INCLUDE_DIR})
    target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_ADLX_HEADERS=1)
    message(STATUS "ADLX headers found: ${AI_Z_ADLX_INCLUDE_DIR} (ADLX integration enabled)")
  else()
    target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_ADLX_HEADERS=0)
    message(STATUS "ADLX headers not found; AMD ADLX telemetry disabled. To enable, set -DAI_Z_ADLX_ROOT=... (expects SDK/Include) or provide ../ADLX/SDK/Include.")
  endif()

  # Intel IGCL (optional)
  set(AI_Z_IGCL_INCLUDE_HINTS "")
  list(APPEND AI_Z_IGCL_INCLUDE_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/igcl")
  if (AI_Z_IGCL_ROOT)
    list(APPEND AI_Z_IGCL_INCLUDE_HINTS "${AI_Z_IGCL_ROOT}")
    list(APPEND AI_Z_IGCL_INCLUDE_HINTS "${AI_Z_IGCL_ROOT}/include")
  endif()

  find_path(AI_Z_IGCL_INCLUDE_DIR
    NAMES igcl_api.h
    HINTS ${AI_Z_IGCL_INCLUDE_HINTS}
  )

  set(AI_Z_IGCL_WRAPPER_HINTS "")
  list(APPEND AI_Z_IGCL_WRAPPER_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/igcl")
  if (AI_Z_IGCL_ROOT)
    list(APPEND AI_Z_IGCL_WRAPPER_HINTS "${AI_Z_IGCL_ROOT}")
    list(APPEND AI_Z_IGCL_WRAPPER_HINTS "${AI_Z_IGCL_ROOT}/src")
    list(APPEND AI_Z_IGCL_WRAPPER_HINTS "${AI_Z_IGCL_ROOT}/Source")
    list(APPEND AI_Z_IGCL_WRAPPER_HINTS "${AI_Z_IGCL_ROOT}/source")
  endif()

  find_file(AI_Z_IGCL_WRAPPER_CPP
    NAMES cApiWrapper.cpp
    HINTS ${AI_Z_IGCL_WRAPPER_HINTS}
  )

  if (AI_Z_IGCL_INCLUDE_DIR AND AI_Z_IGCL_WRAPPER_CPP)
    target_include_directories(aiz-core PUBLIC ${AI_Z_IGCL_INCLUDE_DIR})
    target_sources(aiz-core PRIVATE ${AI_Z_IGCL_WRAPPER_CPP})
    target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_IGCL_HEADERS=1)
    message(STATUS "IGCL headers+wrapper found (Intel telemetry enabled): ${AI_Z_IGCL_INCLUDE_DIR}")
  else()
    target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_IGCL_HEADERS=0)
    message(STATUS "IGCL headers+wrapper not found; Intel IGCL telemetry disabled. To enable, set -DAI_Z_IGCL_ROOT=... or place igcl_api.h + cApiWrapper.cpp under third_party/igcl/.")
  endif()
endif()

# TUI sources and FTXUI (only for CLI build)
if (AI_Z_BUILD_CLI)
  target_sources(ai-z PRIVATE
    src/aiz/tui/ftxui_ui.cpp
    src/aiz/tui/tui_core.cpp
    # Shared helpers
    src/aiz/tui/ncurses_probe.cpp
    src/aiz/tui/ncurses_telemetry.cpp
    src/aiz/tui/ncurses_bench.cpp
    src/aiz/tui/ncurses_bench_rows.cpp
    src/aiz/tui/ncurses_sampler.cpp
    src/aiz/tui/ncurses_bootprobe.cpp
  )

  # FTXUI TUI library (cross-platform, header-only with static libs)
  find_package(ftxui CONFIG REQUIRED)
  target_link_libraries(ai-z PRIVATE ftxui::screen ftxui::dom ftxui::component)
endif()

# Enable real ONNX Runtime benchmarks when headers are available.
# Note: ONNX Runtime C API headers use MSVC-specific calling conventions that
# don't work with MinGW/GCC, so we skip them on MSYS2/MinGW builds.
option(AI_Z_ENABLE_ONNXRUNTIME "Enable ONNX Runtime C API header detection" ON)

set(AI_Z_ONNXRUNTIME_INCLUDE_HINTS "")
if (AI_Z_ENABLE_ONNXRUNTIME AND NOT MINGW)
  list(APPEND AI_Z_ONNXRUNTIME_INCLUDE_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
endif()
if (AI_Z_ONNXRUNTIME_ROOT)
  list(APPEND AI_Z_ONNXRUNTIME_INCLUDE_HINTS "${AI_Z_ONNXRUNTIME_ROOT}/include")
endif()

if (AI_Z_ENABLE_ONNXRUNTIME AND NOT MINGW)
  find_path(AI_Z_ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_c_api.h
    HINTS ${AI_Z_ONNXRUNTIME_INCLUDE_HINTS}
    PATHS /usr/local/include /usr/include
  )
else()
  set(AI_Z_ONNXRUNTIME_INCLUDE_DIR "")
endif()

if (AI_Z_ONNXRUNTIME_INCLUDE_DIR)
  target_include_directories(aiz-core PUBLIC ${AI_Z_ONNXRUNTIME_INCLUDE_DIR})
  target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_ONNXRUNTIME_C_API=1)
  message(STATUS "ONNX Runtime C API headers found: ${AI_Z_ONNXRUNTIME_INCLUDE_DIR} (real ORT benchmarks enabled)")
elseif (MINGW)
  message(STATUS "ONNX Runtime C API headers skipped on MinGW (MSVC-specific calling conventions not supported)")
else()
  message(STATUS "ONNX Runtime C API headers not found; ORT benchmarks will use lightweight proxy loops. Set -DAI_Z_ONNXRUNTIME_ROOT=... to enable real ORT execution.")
endif()

if (AI_Z_ENABLE_LIBTORCH)
  find_package(Torch REQUIRED)
  target_link_libraries(ai-z PRIVATE ${TORCH_LIBRARIES})
  target_compile_definitions(ai-z PRIVATE AI_Z_HAS_TORCH)
endif()

if (AI_Z_ENABLE_CUDA)
  # CUDA is runtime-loaded via libcuda (driver API). No compile-time CUDA toolkit required.
  target_sources(aiz-core PRIVATE
    src/aiz/bench/pcie_bandwidth_cuda.cpp
    src/aiz/bench/gpu_compute_cuda.cpp
  )
endif()

if (AI_Z_ENABLE_OPENCL)
  find_package(OpenCL QUIET)
  if (NOT OpenCL_FOUND)
    message(WARNING "AI_Z_ENABLE_OPENCL=ON but OpenCL not found; disabling OpenCL backend. Configure with -DAI_Z_ENABLE_OPENCL=OFF to silence.")
    set(AI_Z_ENABLE_OPENCL OFF CACHE BOOL "Enable OpenCL benchmarks/backends" FORCE)
  else()
    # Runtime-loaded; only need headers at build time.
    if (TARGET OpenCL::OpenCL)
      get_target_property(AI_Z_OPENCL_INCLUDES OpenCL::OpenCL INTERFACE_INCLUDE_DIRECTORIES)
      if (AI_Z_OPENCL_INCLUDES)
        target_include_directories(aiz-core PUBLIC ${AI_Z_OPENCL_INCLUDES})
      endif()
    else()
      target_include_directories(aiz-core PUBLIC ${OpenCL_INCLUDE_DIRS})
    endif()
  endif()
endif()

if (AI_Z_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if (NOT Vulkan_FOUND)
    message(WARNING "AI_Z_ENABLE_VULKAN=ON but Vulkan not found; disabling Vulkan backend. Configure with -DAI_Z_ENABLE_VULKAN=OFF to silence.")
    set(AI_Z_ENABLE_VULKAN OFF CACHE BOOL "Enable Vulkan benchmarks/backends" FORCE)
  else()
    # Runtime-loaded; only need headers at build time.
    if (TARGET Vulkan::Vulkan)
      get_target_property(AI_Z_VULKAN_INCLUDES Vulkan::Vulkan INTERFACE_INCLUDE_DIRECTORIES)
      if (AI_Z_VULKAN_INCLUDES)
        target_include_directories(aiz-core PUBLIC ${AI_Z_VULKAN_INCLUDES})
      endif()
    else()
      target_include_directories(aiz-core PUBLIC ${Vulkan_INCLUDE_DIRS})
    endif()

    # Optional: shaderc enables runtime GLSL->SPIR-V compilation for Vulkan compute benchmarks.
    # Install via distro package (e.g. libshaderc-dev) to enable.
    find_package(shaderc CONFIG QUIET)
    set(AI_Z_SHADERC_FOUND FALSE)
    if (shaderc_FOUND)
      set(AI_Z_SHADERC_FOUND TRUE)
      if (TARGET shaderc::shaderc)
        target_link_libraries(aiz-core PUBLIC shaderc::shaderc)
      elseif (TARGET shaderc_shared)
        target_link_libraries(aiz-core PUBLIC shaderc_shared)
      elseif (TARGET shaderc)
        target_link_libraries(aiz-core PUBLIC shaderc)
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
        target_include_directories(aiz-core PUBLIC ${SHADERC_INCLUDE_DIR})
        target_link_libraries(aiz-core PUBLIC ${SHADERC_LIBRARY})
      endif()
    endif()

    if (AI_Z_SHADERC_FOUND)
      target_compile_definitions(aiz-core PUBLIC AI_Z_HAS_SHADERC)
    else()
      message(STATUS "shaderc not found; runtime GLSL->SPIR-V compilation disabled (Vulkan FP32 uses embedded SPIR-V).")
    endif()
  endif()
endif()

# Opt-in warnings.
if (AI_Z_ENABLE_WARNINGS)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if (AI_Z_BUILD_CLI)
      target_compile_options(ai-z PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_options(aiz-core PRIVATE -Wall -Wextra -Wpedantic)
  elseif (MSVC)
    if (AI_Z_BUILD_CLI)
      target_compile_options(ai-z PRIVATE /W4)
    endif()
    target_compile_options(aiz-core PRIVATE /W4)
  endif()
endif()

if (BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Install (CLI only)
if (AI_Z_BUILD_CLI)
  set(AI_Z_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
  if (AI_Z_INSTALL_BINDIR STREQUAL "")
    set(AI_Z_INSTALL_BINDIR "bin")
  endif()

  install(TARGETS ai-z
    RUNTIME DESTINATION "${AI_Z_INSTALL_BINDIR}"
  )

  # Windows: best-effort staging of Intel Level Zero loader next to ai-z.exe.
  # IGCL loads ze_loader.dll dynamically (not a link-time dependency), but having it
  # app-local improves portability and enables Intel PCIe telemetry on systems that
  # don't already have the loader on PATH.
  if (AI_Z_PLATFORM_WINDOWS)
    set(AI_Z_ZE_LOADER_HINTS
      "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin"
      "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin"
    )
    if (DEFINED ENV{VCPKG_INSTALLED_DIR})
      list(APPEND AI_Z_ZE_LOADER_HINTS "$ENV{VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
      list(APPEND AI_Z_ZE_LOADER_HINTS "$ENV{VCPKG_INSTALLED_DIR}/x64-windows/bin")
    endif()

    find_file(AI_Z_ZE_LOADER_DLL
      NAMES ze_loader.dll
      HINTS ${AI_Z_ZE_LOADER_HINTS}
    )

    if (AI_Z_ZE_LOADER_DLL)
      add_custom_command(TARGET ai-z POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${AI_Z_ZE_LOADER_DLL}"
          "$<TARGET_FILE_DIR:ai-z>/ze_loader.dll"
        VERBATIM
      )
      install(FILES "${AI_Z_ZE_LOADER_DLL}"
        DESTINATION "${AI_Z_INSTALL_BINDIR}"
        OPTIONAL
      )
    endif()
  endif()

  # Windows: bundle MSVC runtime DLLs app-locally when installing.
  # This makes portable zips/installers runnable on clean machines without requiring
  # the Visual C++ Redistributable to be preinstalled.
  if (MSVC)
    include(InstallRequiredSystemLibraries)
    if (CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
      install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        DESTINATION "${AI_Z_INSTALL_BINDIR}"
      )
    endif()
  endif()

  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/ai-z.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
  )

  # Linux: install desktop entry and icons
  if (AI_Z_PLATFORM_LINUX)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/ai-z.desktop
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )
    
    # Install icons to hicolor theme
    set(AI_Z_ICON_SIZES 16 32 48 256)
    foreach(size ${AI_Z_ICON_SIZES})
      install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/icons/hicolor/${size}x${size}/apps/ai-z.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps
      )
    endforeach()
  endif()

  # Packaging
  set(CPACK_PACKAGE_NAME "ai-z")
  set(CPACK_PACKAGE_VENDOR "ai-z")
  set(CPACK_PACKAGE_CONTACT "ai-z")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++ TUI app for performance timelines (CPU/GPU/Disk/PCIe) and benchmarks")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})

  if (AI_Z_PLATFORM_LINUX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ai-z")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

    # Let dpkg-shlibdeps compute shared-library dependencies when available.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  endif()

  include(CPack)
endif()  # AI_Z_BUILD_CLI
