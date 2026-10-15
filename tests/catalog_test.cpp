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
  mr.meshKey = "prod_arg_foodrations_macro__part_main";
  mr.localTransform.position = {1.0, 2.0, 3.0};
  mr.radius = 42.5;
  mr.xmfPath = "assets/test/part_main-lod0.xmf";
  mr.materialRef = "test.hull";
  mr.detail = DetailClass::XS;
  mr.effect = true;
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
  CHECK(r->meshRefs[0].meshKey == "prod_arg_foodrations_macro__part_main");
  CHECK(r->meshRefs[0].localTransform.position.y == doctest::Approx(2.0));
  CHECK(r->meshRefs[0].radius == doctest::Approx(42.5));
  CHECK(r->meshRefs[0].xmfPath == mr.xmfPath);
  CHECK(r->meshRefs[0].materialRef == mr.materialRef);
  CHECK(r->meshRefs[0].detail == DetailClass::XS);
  CHECK(r->meshRefs[0].effect);
}

TEST_CASE("meshKeyLod names the coarser-LOD sibling, lod 0 is the base path") {
  CHECK(meshKeyLod("a__part_main", 0) == "a__part_main");
  CHECK(meshKeyLod("a__part_main", 2) == "a__part_main-lod2");
}

TEST_CASE("loadFromJson defaults playerBuildable to true when absent") {
  const ModuleCatalog cat = ModuleCatalog::loadFromJson(R"({"modules":[{"id":"m"}]})");
  const ModuleDef* r = cat.find("m");
  REQUIRE(r != nullptr);
  CHECK(r->playerBuildable == true);
}

TEST_CASE("catalog JSON round-trips clearanceVolumes") {
  ModuleDef d;
  d.id = "dock_test";
  ClearanceVolume cv;
  cv.center = {1, 2, 3};
  cv.rotation = {0, 0, 1, 0};  // 180deg about Y
  cv.halfExtents = {45, 45, 600};
  cv.shipSize = "dock_l";
  d.clearanceVolumes.push_back(cv);

  const std::string json = toCatalogJson({d});
  const ModuleCatalog cat = ModuleCatalog::loadFromJson(json);
  const ModuleDef* r = cat.find("dock_test");
  REQUIRE(r != nullptr);
  REQUIRE(r->clearanceVolumes.size() == 1);
  CHECK(r->clearanceVolumes[0].center.z == doctest::Approx(3));
  CHECK(r->clearanceVolumes[0].rotation.y == doctest::Approx(1));
  CHECK(r->clearanceVolumes[0].halfExtents.z == doctest::Approx(600));
  CHECK(r->clearanceVolumes[0].shipSize == "dock_l");
}
