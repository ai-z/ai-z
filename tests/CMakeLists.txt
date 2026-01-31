# Unit tests (Catch2)

# Network-free by default: prefer system Catch2 (v2 or v3).
# Optional fallbacks:
# -DAI_Z_CATCH2_SOURCE_DIR=/path/to/Catch2 (local checkout)
# -DAI_Z_FETCH_CATCH2=ON (FetchContent from GitHub)
option(AI_Z_FETCH_CATCH2 "Fetch Catch2 via FetchContent" OFF)
set(AI_Z_CATCH2_SOURCE_DIR "" CACHE PATH "Path to a local Catch2 source checkout")
option(AI_Z_REQUIRE_CATCH2 "Fail configure if Catch2 is missing (otherwise skip building tests)" ON)

# Test backend selection:
# - auto: use Catch2 if available, else fall back to a tiny built-in runner
# - catch2: require Catch2
# - minitest: always use the built-in runner
set(AI_Z_TEST_BACKEND "auto" CACHE STRING "Test backend: auto|catch2|minitest")
set_property(CACHE AI_Z_TEST_BACKEND PROPERTY STRINGS auto catch2 minitest)

set(_ai_z_use_catch2 OFF)

if (AI_Z_TEST_BACKEND STREQUAL "catch2" OR AI_Z_TEST_BACKEND STREQUAL "auto")
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
    endif()
  endif()

  if (TARGET Catch2::Catch2 OR TARGET Catch2::Catch2WithMain)
    set(_ai_z_use_catch2 ON)
  endif()
endif()

if (AI_Z_TEST_BACKEND STREQUAL "catch2" AND NOT _ai_z_use_catch2)
  message(FATAL_ERROR
    "Catch2 was requested (AI_Z_TEST_BACKEND=catch2) but was not found. Options:\n"
    "- Install Catch2 (e.g. Ubuntu/Debian: sudo apt-get install catch2).\n"
    "- Or set -DAI_Z_CATCH2_SOURCE_DIR=/path/to/Catch2 (local checkout).\n"
    "- Or configure with -DAI_Z_FETCH_CATCH2=ON (requires network access).\n"
    "- Or set -DAI_Z_TEST_BACKEND=minitest to use the built-in test runner."
  )
endif()

if (AI_Z_TEST_BACKEND STREQUAL "auto" AND NOT _ai_z_use_catch2 AND AI_Z_REQUIRE_CATCH2)
  message(WARNING "Catch2 not found; falling back to built-in minitest runner. Set -DAI_Z_TEST_BACKEND=catch2 to require Catch2.")
endif()

add_executable(ai-z-tests
  test_config.cpp
  test_framework.h
  minitest.h
  test_timeline.cpp
  test_version.cpp
  test_snapshot.cpp

  # Compile only small, dependency-light modules.
  ../src/aiz/config/config.cpp
  ../src/aiz/metrics/timeline.cpp
  ../src/aiz/snapshot/json_writer.cpp
)

if (WIN32)
  target_sources(ai-z-tests PRIVATE
    ../src/aiz/platform/windows/config_paths_windows.cpp
  )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(ai-z-tests PRIVATE
    ../src/aiz/platform/linux/config_paths_linux.cpp
  )
endif()

target_compile_features(ai-z-tests PRIVATE cxx_std_20)

target_include_directories(ai-z-tests PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../include
  ${AI_Z_GENERATED_DIR}
)

if (_ai_z_use_catch2)
  target_compile_definitions(ai-z-tests PRIVATE AI_Z_TEST_BACKEND_CATCH2=1)

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

  include(Catch OPTIONAL)
  if (COMMAND catch_discover_tests)
    catch_discover_tests(ai-z-tests)
  else()
    add_test(NAME ai-z-tests COMMAND ai-z-tests)
  endif()
else()
  target_sources(ai-z-tests PRIVATE test_main.cpp)
  add_test(NAME ai-z-tests COMMAND ai-z-tests)
endif()

if (AI_Z_ENABLE_WARNINGS)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ai-z-tests PRIVATE -Wall -Wextra -Wpedantic)
  elseif (MSVC)
    target_compile_options(ai-z-tests PRIVATE /W4)
  endif()
endif()

