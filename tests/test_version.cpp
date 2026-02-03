#include <aiz/version.h>

#include "test_framework.h"

#include <string>

TEST_CASE("Generated version header is present") {
  REQUIRE(std::string(AIZ_VERSION).size() > 0);
  REQUIRE(std::string(AIZ_WEBSITE) == "https://www.ai-z.org");
}
