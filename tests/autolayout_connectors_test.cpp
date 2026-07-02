#include "x4sb/autolayout/connectors.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
ModuleDef structDefTyped(const std::string& id, std::size_t ports, const std::string& type) {
  ModuleDef d;
  d.id = id;
  d.category = Category::Connector;
  for (std::size_t i = 0; i < ports; ++i) {
    ConnectionPoint cp;
    cp.id = "p" + std::to_string(i);
    cp.type = type;
    d.connectionPoints.push_back(cp);
  }
  return d;
}
ModuleDef structDef(const std::string& id, std::size_t ports) {
  return structDefTyped(id, ports, "snap");
}
}  // namespace

TEST_CASE("factionOf: second underscore token") {
  CHECK(factionOf("struct_arg_cross_01_macro") == "arg");
  CHECK(factionOf("struct_bor_base_02_macro") == "bor");
  CHECK(factionOf("prod_gen_energycells_macro") == "gen");
  CHECK(factionOf("nofaction") == "");
}

TEST_CASE("portCount counts connection points") {
  CHECK(portCount(structDef("struct_arg_cross_01_macro", 6)) == 6u);
  CHECK(portCount(structDef("struct_arg_base_01_macro", 2)) == 2u);
}

TEST_CASE("bestHub: smallest connector with >= minPorts, faction-preferred, deterministic") {
  ModuleCatalog c;
  c.add(structDef("struct_arg_cross_02_macro", 5));
  c.add(structDef("struct_arg_cross_03_macro", 4));  // smallest arg hub >=4
  c.add(structDef("struct_arg_cross_01_macro", 6));
  c.add(structDef("struct_bor_cross_01_macro", 4));  // same ports, wrong faction
  c.add(structDef("struct_arg_base_01_macro", 2));   // a span, excluded by minPorts

  const ModuleDef* hub = bestHub(c, "arg", 4);
  REQUIRE(hub != nullptr);
  CHECK(hub->id == "struct_arg_cross_03_macro");  // arg, fewest ports >=4

  // No arg hub big enough -> falls back to any faction (deterministic by id).
  const ModuleDef* big = bestHub(c, "zzz", 5);
  REQUIRE(big != nullptr);
  CHECK(big->id == "struct_arg_cross_02_macro");  // only 5- and 6-port; 5 wins
}

TEST_CASE("bestHubFor: skips a faction hub whose ports cannot host the cart's modules") {
  // Regression: the real catalog's only "gen" hub is struct_gen_venturercross, whose ports
  // are typed "snap venture" — incompatible with plain "snap" modules. bestHub("gen") picked
  // it on faction alone, so every functional module failed to attach and the seed bred empty
  // hubs. bestHubFor must reject a hub it cannot mate, even a faction match.
  ModuleCatalog c;
  c.add(structDefTyped("struct_gen_venturercross_01_macro", 6, "snap venture"));  // faction trap
  c.add(structDefTyped("struct_arg_cross_01_macro", 6, "snap"));                  // compatible

  const ModuleDef* hub = bestHubFor(c, "gen", 3, {"snap"});
  REQUIRE(hub != nullptr);
  CHECK(hub->id == "struct_arg_cross_01_macro");  // NOT the faction-matching venture trap

  // An untagged need ("") is compatible with anything, so the faction preference applies again.
  const ModuleDef* any = bestHubFor(c, "gen", 3, {""});
  REQUIRE(any != nullptr);
  CHECK(any->id == "struct_gen_venturercross_01_macro");

  // Nothing compatible at all -> nullptr (caller degrades to floating).
  CHECK(bestHubFor(c, "gen", 3, {"venture_only_nomatch"}) == nullptr);
}

TEST_CASE("bestSpan: a 2-port connector, faction-preferred") {
  ModuleCatalog c;
  c.add(structDef("struct_bor_base_01_macro", 2));
  c.add(structDef("struct_arg_base_01_macro", 2));
  c.add(structDef("struct_arg_cross_01_macro", 6));  // not a span

  const ModuleDef* span = bestSpan(c, "arg");
  REQUIRE(span != nullptr);
  CHECK(span->id == "struct_arg_base_01_macro");
}

TEST_CASE("bestHub returns nullptr when nothing qualifies") {
  ModuleCatalog c;
  c.add(structDef("struct_arg_base_01_macro", 2));  // only spans
  CHECK(bestHub(c, "arg", 4) == nullptr);
}
