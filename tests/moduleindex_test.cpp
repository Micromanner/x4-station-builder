#include "x4sb/assetpipe/moduleindex.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
const char* kIndexXml = R"(<?xml version="1.0"?>
<index>
  <entry name="Prod_Arg_Foodrations_Macro" value="assets\structures\production\macros\prod_arg_foodrations_macro"/>
  <entry name="struct_arg_cross_01_macro" value="assets\structures\connectionmodules\macros\struct_arg_cross_01_macro"/>
</index>)";
}  // namespace

TEST_CASE("parseModuleIndex maps lower-cased names to normalized .xml paths") {
  const auto idx = parseModuleIndex(kIndexXml);
  REQUIRE(idx.size() == 2);
  const auto it = idx.find("prod_arg_foodrations_macro");  // key lower-cased
  REQUIRE(it != idx.end());
  CHECK(it->second == "assets/structures/production/macros/prod_arg_foodrations_macro.xml");
  CHECK(idx.count("struct_arg_cross_01_macro") == 1);
}

TEST_CASE("parseModuleIndex is fail-safe on malformed or empty entries") {
  CHECK(parseModuleIndex("<not xml").empty());  // unparseable -> empty map
  CHECK(parseModuleIndex("").empty());          // empty input -> empty map
  // entries missing name or value are skipped; the valid one survives
  const auto idx = parseModuleIndex(
      R"(<index><entry value="a\b"/><entry name="keep_macro" value="c\d"/><entry name="nopath"/></index>)");
  REQUIRE(idx.size() == 1);
  CHECK(idx.at("keep_macro") == "c/d.xml");
}

TEST_CASE("parseModuleIndex collapses doubled separators in index values") {
  // Some real X4 index entries use double backslashes; "\\" -> "//" must collapse to "/".
  const auto idx = parseModuleIndex(
      R"(<index><entry name="dbl_macro" value="assets\\structures\\macros\\dbl_macro"/></index>)");
  REQUIRE(idx.count("dbl_macro") == 1);
  CHECK(idx.at("dbl_macro") == "assets/structures/macros/dbl_macro.xml");
}
