#include <aiz/version.h>

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#elif __has_include(<catch2/catch.hpp>)
#include <catch2/catch.hpp>
#else
#error "Catch2 headers not found"
#endif

#include <string>

TEST_CASE("Generated version header is present") {
  REQUIRE(std::string(AIZ_VERSION).size() > 0);
  REQUIRE(std::string(AIZ_WEBSITE) == "https://www.ai-z.org");
}
