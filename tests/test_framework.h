#pragma once

#if defined(AI_Z_TEST_BACKEND_CATCH2)
  #if __has_include(<catch2/catch_test_macros.hpp>)
    #include <catch2/catch_test_macros.hpp>
  #elif __has_include(<catch2/catch.hpp>)
    #include <catch2/catch.hpp>
  #else
    #error "Catch2 headers not found"
  #endif
#else
  #include "minitest.h"
#endif
