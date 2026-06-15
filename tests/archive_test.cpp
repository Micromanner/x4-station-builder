#include "x4sb/archive/archive.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace x4sb;

TEST_CASE("CatIndex computes cumulative offsets and skips malformed lines") {
  const std::string cat =
      "assets/a.xml 10 1208774952 380e1d7ea9d0e38fa17e7e708a01ef28\n"
      "assets/b/c.xmf 25 1615208041 e924f78e40b1ac828c4672224fb25429\n"
      "garbage line without enough fields\n"  // skipped: no numeric size field
      "assets/d.xml 5 1 abc\n";
  const CatIndex idx = CatIndex::parse(cat);

  REQUIRE(idx.entries().size() == 3);
  CHECK(idx.entries()[0].offset == 0);
  CHECK(idx.entries()[0].size == 10);
  CHECK(idx.entries()[1].offset == 10);  // 0 + 10
  CHECK(idx.entries()[2].offset == 35);  // 10 + 25
  CHECK(idx.totalSize() == 40);          // 10 + 25 + 5

  REQUIRE(idx.find("assets/b/c.xmf") != nullptr);
  CHECK(idx.find("assets/b/c.xmf")->offset == 10);
  CHECK(idx.find("missing") == nullptr);
}

TEST_CASE("Archive extracts bytes from a synthetic cat/dat pair; later catalog wins") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "x4sb_archive_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  auto writePair = [&](const std::string& name, const std::string& cat, const std::string& dat) {
    std::ofstream(dir / (name + ".cat"), std::ios::binary) << cat;
    std::ofstream(dir / (name + ".dat"), std::ios::binary) << dat;
  };

  // Base catalog: foo.xml (5 bytes) then bar.txt (3 bytes), concatenated in .dat.
  writePair("01", "foo.xml 5 1 aa\nbar.txt 3 1 bb\n", "HELLObye");
  // Patch catalog overrides foo.xml with new content.
  writePair("02", "foo.xml 6 1 cc\n", "FOOFOO");

  Archive ar;
  CHECK(ar.addCatalog((dir / "01.cat").string()));
  CHECK(ar.addCatalog((dir / "02.cat").string()));

  const auto foo = ar.extract("foo.xml");
  REQUIRE(foo.has_value());
  CHECK(*foo == "FOOFOO");  // 02 layered over 01

  const auto bar = ar.extract("bar.txt");
  REQUIRE(bar.has_value());
  CHECK(*bar == "bye");

  CHECK_FALSE(ar.extract("missing").has_value());

  // _sig signature catalogs are ignored.
  writePair("03_sig", "x 1 1 dd\n", "Z");
  CHECK_FALSE(ar.addCatalog((dir / "03_sig.cat").string()));

  fs::remove_all(dir);
}

TEST_CASE("Archive::sources() returns distinct prefixes in first-seen order") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "x4sb_sources_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  auto writePair = [&](const fs::path& catPath, const fs::path& datPath,
                       const std::string& cat, const std::string& dat) {
    std::ofstream(catPath, std::ios::binary) << cat;
    std::ofstream(datPath, std::ios::binary) << dat;
  };

  // Base catalog 01
  writePair(dir / "01.cat", dir / "01.dat", "foo.xml 3 1 aa\n", "foo");
  // Base catalog 02 — same prefix "", must NOT produce a second "" entry
  writePair(dir / "02.cat", dir / "02.dat", "bar.xml 3 1 bb\n", "bar");
  // DLC catalog
  const fs::path dlcDir = dir / "dlc";
  fs::create_directories(dlcDir);
  writePair(dlcDir / "01.cat", dlcDir / "01.dat", "mod.xml 3 1 cc\n", "mod");

  Archive ar;
  CHECK(ar.addCatalog((dir / "01.cat").string(), ""));
  CHECK(ar.addCatalog((dir / "02.cat").string(), ""));
  CHECK(ar.addCatalog((dlcDir / "01.cat").string(), "extensions/ego_dlc_test/"));

  const auto& srcs = ar.sources();
  REQUIRE(srcs.size() == 2);
  CHECK(srcs[0] == "");
  CHECK(srcs[1] == "extensions/ego_dlc_test/");

  // Adding another catalog under the DLC prefix must NOT add a duplicate.
  writePair(dlcDir / "02.cat", dlcDir / "02.dat", "extra.xml 5 1 dd\n", "extra!");
  CHECK(ar.addCatalog((dlcDir / "02.cat").string(), "extensions/ego_dlc_test/"));
  CHECK(ar.sources().size() == 2);

  fs::remove_all(dir);
}
