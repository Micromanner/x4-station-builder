#include "x4sb/assetpipe/wares.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

namespace {
// Two module wares (the second flagged noplayerblueprint) + one non-module ware
// (an engine) that must be filtered out, + a module ware with no component ref.
const char* kWaresXml = R"(<?xml version="1.0"?>
<wares>
  <ware id="module_arg_prod_foodrations_01" name="{20104,13401}" tags="module">
    <component ref="prod_arg_foodrations_macro"/>
  </ware>
  <ware id="module_par_defence_01" name="{20104,40001}" tags="module noplayerblueprint">
    <component ref="defence_par_m_01_macro"/>
  </ware>
  <ware id="engine_arg_s_01" name="{20101,1}" tags="engine">
    <component ref="engine_arg_s_01_macro"/>
  </ware>
  <ware id="module_broken_01" name="{1,1}" tags="module"/>
</wares>)";

// A DLC overlay wares.xml is a <diff> that <add>s module wares. The first module
// also carries a <production> with nested ingredient <ware ware=.../> refs that
// must NOT be counted as modules.
const char* kDiffWaresXml = R"(<?xml version="1.0"?>
<diff>
  <add sel="/wares/production">
    <method id="boron" name="{1,1}"/>
  </add>
  <add sel="/wares">
    <ware id="module_bor_prod_01" name="{20104,9001}" tags="module">
      <production time="60"><primary><ware ware="energycells" amount="100"/></primary></production>
      <component ref="prod_bor_bofu_macro"/>
    </ware>
    <ware id="module_bor_dock_01" name="{20104,9002}" tags="module noplayerblueprint">
      <component ref="dock_bor_01_macro"/>
    </ware>
  </add>
</diff>)";
}  // namespace

TEST_CASE("parseModuleWares keeps module-tagged wares with a component ref") {
  const auto mods = parseModuleWares(kWaresXml);
  REQUIRE(mods.size() == 2);  // engine filtered; broken (no component) dropped

  CHECK(mods[0].wareId == "module_arg_prod_foodrations_01");
  CHECK(mods[0].nameRef == "{20104,13401}");
  CHECK(mods[0].macroRef == "prod_arg_foodrations_macro");
  CHECK(mods[0].playerBuildable == true);

  CHECK(mods[1].wareId == "module_par_defence_01");
  CHECK(mods[1].playerBuildable == false);  // noplayerblueprint
}

TEST_CASE("parseModuleWares is fail-safe on malformed input") {
  CHECK(parseModuleWares("<not xml").empty());
  CHECK(parseModuleWares("").empty());
}

TEST_CASE("parseModuleWares reads diff-added module wares and ignores ingredient refs") {
  const auto mods = parseModuleWares(kDiffWaresXml);
  REQUIRE(mods.size() == 2);  // two module wares; the nested energycells ingredient is NOT counted
  CHECK(mods[0].wareId == "module_bor_prod_01");
  CHECK(mods[0].macroRef == "prod_bor_bofu_macro");
  CHECK(mods[0].playerBuildable == true);
  CHECK(mods[1].wareId == "module_bor_dock_01");
  CHECK(mods[1].playerBuildable == false);
}
