#include <aiz/metrics/timeline.h>

#include "test_framework.h"

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
