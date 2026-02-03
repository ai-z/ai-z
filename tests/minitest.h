#pragma once

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aiz::minitest {

struct Failure final : std::exception {
  std::string message;

  explicit Failure(std::string msg) : message(std::move(msg)) {}

  const char* what() const noexcept override { return message.c_str(); }
};

using TestFn = void (*)();

struct TestCase {
  std::string_view name;
  TestFn fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline bool registerTest(std::string_view name, TestFn fn) {
  registry().push_back(TestCase{name, fn});
  return true;
}

inline int runAll() {
  int failed = 0;
  for (const auto& t : registry()) {
    try {
      t.fn();
      std::cout << "[PASS] " << t.name << "\n";
    } catch (const Failure& f) {
      ++failed;
      std::cout << "[FAIL] " << t.name << "\n";
      std::cout << "       " << f.what() << "\n";
    } catch (const std::exception& e) {
      ++failed;
      std::cout << "[FAIL] " << t.name << "\n";
      std::cout << "       unhandled std::exception: " << e.what() << "\n";
    } catch (...) {
      ++failed;
      std::cout << "[FAIL] " << t.name << "\n";
      std::cout << "       unhandled unknown exception\n";
    }
  }

  if (failed == 0) {
    std::cout << "All tests passed (" << registry().size() << ")\n";
    return 0;
  }

  std::cout << failed << " test(s) failed (" << registry().size() << " total)\n";
  return 1;
}

}  // namespace aiz::minitest

#define AIZ_MINITEST_CONCAT_IMPL(a, b) a##b
#define AIZ_MINITEST_CONCAT(a, b) AIZ_MINITEST_CONCAT_IMPL(a, b)

#define AIZ_MINITEST_TEST_CASE_IMPL(id, name_literal)                                    \
  static void AIZ_MINITEST_CONCAT(aiz_minitest_fn_, id)();                               \
  static const bool AIZ_MINITEST_CONCAT(aiz_minitest_reg_, id) =                         \
      ::aiz::minitest::registerTest((name_literal), &AIZ_MINITEST_CONCAT(aiz_minitest_fn_, id)); \
  static void AIZ_MINITEST_CONCAT(aiz_minitest_fn_, id)()

#define TEST_CASE(name_literal) AIZ_MINITEST_TEST_CASE_IMPL(__COUNTER__, name_literal)

#define REQUIRE(...)                                                                     \
  do {                                                                                  \
    if (!(__VA_ARGS__)) {                                                               \
      throw ::aiz::minitest::Failure(std::string("REQUIRE failed: ") + #__VA_ARGS__ +  \
                                    " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")"); \
    }                                                                                   \
  } while (0)
