#include "x4sb/autolayout/autolayout.hpp"
#include "x4sb/autolayout/search.hpp"
#include "x4sb/data/math.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace x4sb;

namespace {
const AABB kUnit{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};

ModuleDef boxDef(const std::string& id, AABB box = kUnit) {
  ModuleDef d;
  d.id = id;
  d.aabb = box;
  return d;
}
PlacedModule at(const std::string& defId, Vec3 pos) {
  PlacedModule m;
  m.defId = defId;
  m.worldTransform.position = pos;
  return m;
}
}  // namespace

TEST_CASE("layoutLegal: well-separated in-plot station is legal") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station s;
  s.add(at("b", {0, 0, 0}));
  s.add(at("b", {10, 0, 0}));
  AutoLayoutOptions opts;
  CHECK(layoutLegal(s, c, opts));
}

TEST_CASE("layoutLegal: non-adjacent overlap is illegal") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station s;
  s.add(at("b", {0, 0, 0}));
  s.add(at("b", {0, 0, 0}));  // coincident, unlinked
  AutoLayoutOptions opts;
  CHECK_FALSE(layoutLegal(s, c, opts));
}

TEST_CASE("layoutLegal: linked neighbours that touch at the joint are legal") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station s;
  PlacedModule a = at("b", {0, 0, 0});
  PlacedModule d = at("b", {1, 0, 0});  // touching face at x=0.5 (overlaps() counts touch)
  const InstanceId ida = s.add(a);
  const InstanceId idd = s.add(d);
  // Link them both ways so the adjacency skip applies.
  s.find(ida)->links.push_back(Link{"p", idd, "q"});
  s.find(idd)->links.push_back(Link{"q", ida, "p"});
  AutoLayoutOptions opts;
  CHECK(layoutLegal(s, c, opts));
}

TEST_CASE("layoutLegal: a body outside the plot is illegal") {
  ModuleCatalog c;
  c.add(boxDef("b"));
  Station s;
  s.add(at("b", {0, 0, 0}));
  AutoLayoutOptions opts;
  opts.plotHalfExtent = 5.0;
  s.add(at("b", {100, 0, 0}));  // beyond +/-5
  CHECK_FALSE(layoutLegal(s, c, opts));
}

TEST_CASE("layoutLegal: a body inside a dock corridor is illegal") {
  ModuleCatalog c;
  ModuleDef dock = boxDef("dock");
  ClearanceVolume cv;  // a corridor 10 long along +Z, centred 5 out
  cv.center = {0, 0, 5};
  cv.halfExtents = {0.4, 0.4, 5};
  dock.clearanceVolumes.push_back(cv);
  c.add(dock);
  c.add(boxDef("b"));

  Station blocked;
  blocked.add(at("dock", {0, 0, 0}));
  blocked.add(at("b", {0, 0, 5}));  // sits in the corridor
  AutoLayoutOptions opts;
  CHECK_FALSE(layoutLegal(blocked, c, opts));

  Station clear;
  clear.add(at("dock", {0, 0, 0}));
  clear.add(at("b", {20, 0, 0}));  // out of the corridor
  CHECK(layoutLegal(clear, c, opts));
}
