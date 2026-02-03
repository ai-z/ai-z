#if defined(AI_Z_TEST_BACKEND_CATCH2)
  #if __has_include(<catch2/catch_all.hpp>)
    #define CATCH_CONFIG_MAIN
    #include <catch2/catch_all.hpp>
  #elif __has_include(<catch2/catch.hpp>)
    #define CATCH_CONFIG_MAIN
    #include <catch2/catch.hpp>
  #else
    #error "Catch2 headers not found"
  #endif
#else
  #include "minitest.h"

int main() {
  return ::aiz::minitest::runAll();
}
#endif
