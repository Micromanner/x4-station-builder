#include "x4sb/platform/platform.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("constructionPlanDir composes <profileId>/constructionplan onto the user dir") {
  const auto p = platform::constructionPlanDir("12345678");
  REQUIRE_FALSE(p.empty());  // USERPROFILE/HOME is set in any normal environment
  CHECK(p.filename() == std::filesystem::path("constructionplan"));
  CHECK(p.parent_path().filename() == std::filesystem::path("12345678"));
}

TEST_CASE("x4UserDataDir resolves to an X4 data folder") {
  const auto d = platform::x4UserDataDir();
  REQUIRE_FALSE(d.empty());
  CHECK(d.filename() == std::filesystem::path("X4"));
}
