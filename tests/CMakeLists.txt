# Unit tests (Catch2)

# Network-free by default: prefer system Catch2 (v2 or v3).
# Optional fallbacks:
# -DAI_Z_CATCH2_SOURCE_DIR=/path/to/Catch2 (local checkout)
# -DAI_Z_FETCH_CATCH2=ON (FetchContent from GitHub)
option(AI_Z_FETCH_CATCH2 "Fetch Catch2 via FetchContent" OFF)
set(AI_Z_CATCH2_SOURCE_DIR "" CACHE PATH "Path to a local Catch2 source checkout")

if (AI_Z_CATCH2_SOURCE_DIR)
  if (EXISTS "${AI_Z_CATCH2_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${AI_Z_CATCH2_SOURCE_DIR}" catch2-build)
  else()
    message(FATAL_ERROR "AI_Z_CATCH2_SOURCE_DIR is set but does not look like a Catch2 checkout: ${AI_Z_CATCH2_SOURCE_DIR}")
  endif()
else()
  find_package(Catch2 3 CONFIG QUIET)
  if (NOT Catch2_FOUND)
    find_package(Catch2 2 CONFIG QUIET)
  endif()

  if (NOT Catch2_FOUND AND AI_Z_FETCH_CATCH2)
    include(FetchContent)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)
  elseif(NOT Catch2_FOUND)
    message(FATAL_ERROR
      "Catch2 was not found. Install it (e.g. Debian/Ubuntu: sudo apt-get install catch2),\n"
      "or set -DAI_Z_CATCH2_SOURCE_DIR=/path/to/Catch2,\n"
      "or configure with -DAI_Z_FETCH_CATCH2=ON (requires network access)."
    )
  endif()
endif()

add_executable(ai-z-tests
  test_config.cpp
  test_timeline.cpp
  test_version.cpp

  # Compile only small, dependency-light modules.
  ../src/aiz/config/config.cpp
  ../src/aiz/metrics/timeline.cpp
)

target_compile_features(ai-z-tests PRIVATE cxx_std_20)

target_include_directories(ai-z-tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include
  ${AI_Z_GENERATED_DIR}
)

if (TARGET Catch2::Catch2WithMain)
  target_link_libraries(ai-z-tests PRIVATE Catch2::Catch2WithMain)
else()
  target_sources(ai-z-tests PRIVATE test_main.cpp)
  if (TARGET Catch2::Catch2)
    target_link_libraries(ai-z-tests PRIVATE Catch2::Catch2)
  else()
    message(FATAL_ERROR "Catch2 found but no known targets (expected Catch2::Catch2WithMain or Catch2::Catch2)")
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ai-z-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(Catch OPTIONAL)
if (COMMAND catch_discover_tests)
  catch_discover_tests(ai-z-tests)
else()
  add_test(NAME ai-z-tests COMMAND ai-z-tests)
endif()
