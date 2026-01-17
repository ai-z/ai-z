#include <aiz/metrics/timeline.h>

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#elif __has_include(<catch2/catch.hpp>)
#include <catch2/catch.hpp>
#else
#error "Catch2 headers not found"
#endif

TEST_CASE("Timeline behaves as a ring buffer (oldest->newest)") {
  aiz::Timeline t(3);

  REQUIRE(t.capacity() == 3);
  REQUIRE(t.size() == 0);

  t.push(1.0);
  t.push(2.0);
  REQUIRE(t.size() == 2);
  REQUIRE(t.values() == std::vector<double>{1.0, 2.0});

  t.push(3.0);
  REQUIRE(t.size() == 3);
  REQUIRE(t.values() == std::vector<double>{1.0, 2.0, 3.0});

  t.push(4.0);
  REQUIRE(t.size() == 3);
  REQUIRE(t.values() == std::vector<double>{2.0, 3.0, 4.0});
}

TEST_CASE("Timeline with zero capacity is safe") {
  aiz::Timeline t(0);
  REQUIRE(t.capacity() == 0);
  REQUIRE(t.size() == 0);

  t.push(123.0);
  REQUIRE(t.size() == 0);
  REQUIRE(t.values().empty());
}
