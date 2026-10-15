#include "station_gpu.hpp"

#include <doctest/doctest.h>

#include <cstddef>

using x4sb::editor::DedupTable;
using x4sb::editor::GpuMaterial;
using x4sb::editor::GpuUnlitMaterial;

TEST_CASE("DedupTable stores each distinct material once") {
  DedupTable<GpuMaterial> table;
  GpuMaterial hull;
  hull.albedoMap = 7;
  GpuMaterial glass = hull;
  glass.alphaTest = 1;

  const auto first = table.add(hull);
  CHECK(table.add(glass) != first);
  CHECK(table.add(hull) == first);
  REQUIRE(table.items().size() == 2);
  CHECK(table.items()[first].albedoMap == 7);
}

TEST_CASE("DedupTable tells unlit materials apart by any field") {
  DedupTable<GpuUnlitMaterial> table;
  GpuUnlitMaterial sign;
  GpuUnlitMaterial forcefield = sign;
  forcefield.isForcefield = 1;
  CHECK(table.add(sign) == 0);
  CHECK(table.add(forcefield) == 1);
  CHECK(table.add(sign) == 0);
}

TEST_CASE("DrawData and DrawCommand match their GPU layouts") {
  using x4sb::editor::DrawCommand;
  using x4sb::editor::DrawData;
  CHECK(offsetof(DrawData, lodFade) == 4);
  CHECK(offsetof(DrawData, uvSlot) == 12);
  CHECK(offsetof(DrawCommand, baseVertex) == 12);
  CHECK(offsetof(DrawCommand, baseInstance) == 16);
}
