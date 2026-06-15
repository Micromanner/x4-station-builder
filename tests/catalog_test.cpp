#include "x4sb/data/catalog.hpp"

#include <doctest/doctest.h>

using namespace x4sb;

TEST_CASE("toCatalogJson round-trips through loadFromJson") {
  ModuleDef d;
  d.id = "prod_arg_foodrations_macro";
  d.wareId = "module_arg_prod_foodrations_01";
  d.nameRef = "{20104,13401}";
  d.faction = "argon";
  d.category = Category::Production;
  d.playerBuildable = false;
  d.aabb = {{-9.0, -18.0, -27.0}, {11.0, 22.0, 33.0}};

  ConnectionPoint cp;
  cp.id = "ConnectionSnap001";
  cp.localPosition = {0.0, 0.0, 200.0};
  cp.localRotation = {0.0, 0.0, -1.0, 0.0};
  cp.type = "snap";
  d.connectionPoints.push_back(cp);

  MeshRef mr;
  mr.gltfPath = "meshes/prod_arg_foodrations_macro__part_main.gltf";
  mr.localTransform.position = {1.0, 2.0, 3.0};
  d.meshRefs.push_back(mr);

  const ModuleCatalog cat = ModuleCatalog::loadFromJson(toCatalogJson({d}));
  REQUIRE(cat.size() == 1);
  const ModuleDef* r = cat.find("prod_arg_foodrations_macro");
  REQUIRE(r != nullptr);
  CHECK(r->wareId == "module_arg_prod_foodrations_01");
  CHECK(r->nameRef == "{20104,13401}");
  CHECK(r->faction == "argon");
  CHECK(r->category == Category::Production);
  CHECK(r->playerBuildable == false);
  CHECK(r->aabb.min.x == doctest::Approx(-9.0));
  CHECK(r->aabb.max.z == doctest::Approx(33.0));
  REQUIRE(r->connectionPoints.size() == 1);
  CHECK(r->connectionPoints[0].id == "ConnectionSnap001");
  CHECK(r->connectionPoints[0].localPosition.z == doctest::Approx(200.0));
  CHECK(r->connectionPoints[0].localRotation.y == doctest::Approx(-1.0));
  REQUIRE(r->meshRefs.size() == 1);
  CHECK(r->meshRefs[0].gltfPath == "meshes/prod_arg_foodrations_macro__part_main.gltf");
  CHECK(r->meshRefs[0].localTransform.position.y == doctest::Approx(2.0));
}

TEST_CASE("loadFromJson defaults playerBuildable to true when absent") {
  const ModuleCatalog cat = ModuleCatalog::loadFromJson(R"({"modules":[{"id":"m"}]})");
  const ModuleDef* r = cat.find("m");
  REQUIRE(r != nullptr);
  CHECK(r->playerBuildable == true);
}
