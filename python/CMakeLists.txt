# Python bindings for AI-Z metrics (Linux only)
#
# This CMakeLists.txt is used by scikit-build-core for pip installs.
# It includes the parent CMakeLists.txt to get the aiz-core library,
# then builds the Python extension module linking against it.

cmake_minimum_required(VERSION 3.16)

project(aiz-python LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disable CLI build - we only need the aiz-core library for Python bindings
# This skips FTXUI dependency which isn't available in manylinux containers
set(AI_Z_BUILD_CLI OFF CACHE BOOL "" FORCE)

# Include the parent project to build aiz-core library
# This avoids duplicating source lists
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/ai-z-build EXCLUDE_FROM_ALL)

# Find Python and pybind11
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

# Build the Python extension module - links against aiz-core from parent
pybind11_add_module(_aiz_core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings.cpp
)

# Link against the shared aiz-core library
target_link_libraries(_aiz_core PRIVATE aiz-core)

# Install the module
install(TARGETS _aiz_core DESTINATION aiz)
