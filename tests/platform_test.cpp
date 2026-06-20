#include "x4sb/platform/platform.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <thread>

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

TEST_CASE("profileIdsIn returns digit-named subdirs, newest mtime first") {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path() / "x4sb_profile_test";
  std::error_code ec;
  fs::remove_all(base, ec);
  // 111 created first, then 222: 222 has the newer directory mtime. A real time
  // gap (not an explicit last_write_time set) is used because setting a
  // directory's mtime is denied on some Windows filesystems — the standard
  // library opens the dir without FILE_FLAG_BACKUP_SEMANTICS. The gap must clear
  // this FS's ~1s directory-mtime granularity (probed: <1s flaky, 1s+ reliable).
  fs::create_directories(base / "111", ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  fs::create_directories(base / "222", ec);
  fs::create_directories(base / "notnumeric", ec);  // non-digit dir: excluded
  {
    std::ofstream f(base / "999");
    f << "x";
  }  // digit-named FILE: excluded

  const auto ids = platform::profileIdsIn(base);
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "222");  // newest first
  CHECK(ids[1] == "111");

  fs::remove_all(base, ec);
}

TEST_CASE("profileIdsIn is empty (no throw) for a missing dir") {
  const auto ids =
      platform::profileIdsIn(std::filesystem::temp_directory_path() / "x4sb_missing_zzz");
  CHECK(ids.empty());
}
